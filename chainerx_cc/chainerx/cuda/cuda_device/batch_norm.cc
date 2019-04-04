#include "chainerx/cuda/cuda_device.h"

#include <cstdint>
#include <memory>
#include <utility>

#include <cudnn.h>

#include "chainerx/array.h"
#include "chainerx/axes.h"
#include "chainerx/backend_util.h"
#include "chainerx/cuda/cuda_set_device_scope.h"
#include "chainerx/cuda/cudnn.h"
#include "chainerx/cuda/op_regist.h"
#include "chainerx/device.h"
#include "chainerx/dtype.h"
#include "chainerx/error.h"
#include "chainerx/macro.h"
#include "chainerx/routines/creation.h"
#include "chainerx/routines/normalization.h"
#include "chainerx/scalar.h"
#include "chainerx/shape.h"

namespace chainerx {
namespace cuda {
namespace {

// TODO(sonots): Support other than 4, 5 dimensional arrays by reshaping into 4-dimensional arrays as Chainer does.
cudnnBatchNormMode_t GetBatchNormMode(const Axes& axis) {
    if (axis.ndim() == 1 && axis[0] == 0) {  // (1, channels, (depth, )height, width)
        return CUDNN_BATCHNORM_PER_ACTIVATION;
    }
    if ((axis.ndim() == 3 && axis[0] == 0 && axis[1] == 2 && axis[2] == 3) ||
        (axis.ndim() == 4 && axis[0] == 0 && axis[1] == 2 && axis[2] == 3 && axis[3] == 4)) {  // (1, channels, (1, )1, 1)
        // TODO(hvy): Consider CUDNN_BATCHNORM_SPATIAL_PERSISTENT if we can afford to check for overflow, with or without blocking.
        return CUDNN_BATCHNORM_SPATIAL;
    }
    throw DimensionError{"Invalid axis for BatchNorm using cuDNN ", axis, ". Expected 1, 3 or 4 dimensions."};
}

// Helper function to update the running mean and running variance.
void UpdateRunning(const Array& running, const Array& running_updated) {
    CHAINERX_ASSERT(running.IsContiguous());
    CHAINERX_ASSERT(running_updated.IsContiguous());
    CHAINERX_ASSERT(&running.device() == &running_updated.device());
    CHAINERX_ASSERT(
            (running.dtype() == running_updated.dtype()) ==
            (internal::GetRawOffsetData(running) == internal::GetRawOffsetData(running_updated)));

    if (running.dtype() == running_updated.dtype()) {
        // Assume that running already holds the updated values.
        return;
    }

    // The running values must be written back.
    const Array& running_casted_back = running_updated.AsType(running.dtype());
    Device& device = running.device();
    device.MemoryCopyFrom(
            internal::GetRawOffsetData(running), internal::GetRawOffsetData(running_casted_back), running.GetNBytes(), device);
}

class CudnnBNTensorDescriptor {
public:
    CudnnBNTensorDescriptor(const cuda_internal::CudnnTensorDescriptor& x_desc, cudnnBatchNormMode_t mode) : CudnnBNTensorDescriptor{} {
        CheckCudnnError(cudnnDeriveBNTensorDescriptor(desc_, *x_desc, mode));
    }

    CudnnBNTensorDescriptor(const CudnnBNTensorDescriptor&) = delete;
    CudnnBNTensorDescriptor& operator=(const CudnnBNTensorDescriptor&) = delete;

    CudnnBNTensorDescriptor(CudnnBNTensorDescriptor&&) = delete;
    CudnnBNTensorDescriptor& operator=(CudnnBNTensorDescriptor&&) = delete;

    ~CudnnBNTensorDescriptor() {
        if (desc_ != nullptr) {
            CheckCudnnError(cudnnDestroyTensorDescriptor(desc_));
        }
    }

    cudnnTensorDescriptor_t descriptor() const { return desc_; }
    cudnnTensorDescriptor_t operator*() const { return desc_; }

    Dtype GetDtype() {
        cudnnDataType_t cudnn_dtype{};
        int ndim{};

        CheckCudnnError(cudnnGetTensorNdDescriptor(desc_, 0, &cudnn_dtype, &ndim, nullptr, nullptr));

        switch (cudnn_dtype) {
            case CUDNN_DATA_HALF:
                return Dtype::kFloat16;
            case CUDNN_DATA_FLOAT:
                return Dtype::kFloat32;
            case CUDNN_DATA_DOUBLE:
                return Dtype::kFloat64;
            default:
                throw DtypeError{"Unsupported cudnn data type: ", cudnn_dtype};
        }
    }

private:
    CudnnBNTensorDescriptor() { CheckCudnnError(cudnnCreateTensorDescriptor(&desc_)); }
    cudnnTensorDescriptor_t desc_{};
};

struct CudaBatchNormState {
    CudaBatchNormState(Array x_cont, Array x_mean, Array x_inv_std)
        : x_cont{std::move(x_cont)}, x_mean{std::move(x_mean)}, x_inv_std{std::move(x_inv_std)} {}

    Array x_cont;
    Array x_mean;
    Array x_inv_std;
};

}  // namespace

class CudaBatchNormForwardOp : public BatchNormForwardOp {
public:
    void Call(
            const Array& x,
            const Array& gamma,
            const Array& beta,
            const Array& running_mean,
            const Array& running_var,
            Scalar eps,
            Scalar decay,
            const Axes& axis,
            const Array& out,
            nonstd::optional<std::shared_ptr<void>>& state) override {
        if (CHAINERX_DEBUG) {
            Shape reduced_shape = internal::ReduceShape(x.shape(), axis, true);
            CHAINERX_ASSERT(gamma.shape() == reduced_shape);
            CHAINERX_ASSERT(beta.shape() == reduced_shape);

            int64_t reduced_total_size = reduced_shape.GetTotalSize();
            CHAINERX_ASSERT(running_mean.GetTotalSize() == reduced_total_size);
            CHAINERX_ASSERT(running_var.GetTotalSize() == reduced_total_size);

            CHAINERX_ASSERT(&x.device() == &gamma.device());
            CHAINERX_ASSERT(&x.device() == &beta.device());
            CHAINERX_ASSERT(&x.device() == &running_mean.device());
            CHAINERX_ASSERT(&x.device() == &running_var.device());

            CHAINERX_ASSERT(GetKind(x.dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(gamma.dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(beta.dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(running_mean.dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(running_var.dtype()) == DtypeKind::kFloat);
        }

        if (static_cast<double>(eps) < CUDNN_BN_MIN_EPSILON) {
            throw CudnnError{"Minimum allowed epsilon is ", CUDNN_BN_MIN_EPSILON, " but found ", eps, "."};
        }
        if (!running_mean.IsContiguous()) {
            throw DeviceError{"Running mean must to be contiguous for cuDNN to update it in-place."};
        }
        if (!running_var.IsContiguous()) {
            throw DeviceError{"Running variance must to be contiguous for cuDNN to update it in-place."};
        }

        CudaDevice& device = dynamic_cast<CudaDevice&>(x.device());
        CudaSetDeviceScope scope{device.index()};

        Array x_cont = internal::AsContiguous(x);
        cuda_internal::CudnnTensorDescriptor x_desc{x_cont};

        cudnnBatchNormMode_t mode = GetBatchNormMode(axis);

        CudnnBNTensorDescriptor gamma_beta_mean_var_desc{x_desc, mode};
        Dtype gamma_beta_mean_var_dtype = gamma_beta_mean_var_desc.GetDtype();

        Array gamma_casted_cont = internal::AsContiguous(gamma, gamma_beta_mean_var_dtype);
        Array beta_casted_cont = internal::AsContiguous(beta, gamma_beta_mean_var_dtype);

        CHAINERX_ASSERT(running_mean.IsContiguous());
        CHAINERX_ASSERT(running_var.IsContiguous());

        // Convert parameter dtypes if they do not match the dtype expected by cuDNN.
        const Array& running_mean_casted =
                running_mean.dtype() != gamma_beta_mean_var_dtype ? running_mean.AsType(gamma_beta_mean_var_dtype) : running_mean;
        const Array& running_var_casted =
                running_var.dtype() != gamma_beta_mean_var_dtype ? running_var.AsType(gamma_beta_mean_var_dtype) : running_var;

        Array x_mean = EmptyLike(gamma_casted_cont, device);
        Array x_inv_std = EmptyLike(gamma_casted_cont, device);

        Dtype dtype = x.dtype();

        device.cudnn_handle().Call(
                cudnnBatchNormalizationForwardTraining,
                mode,
                cuda_internal::GetCudnnCoefficientPtr<1>(dtype),
                cuda_internal::GetCudnnCoefficientPtr<0>(dtype),
                *x_desc,
                internal::GetRawOffsetData(x_cont),
                *x_desc,
                internal::GetRawOffsetData(out),
                *gamma_beta_mean_var_desc,
                internal::GetRawOffsetData(gamma_casted_cont),
                internal::GetRawOffsetData(beta_casted_cont),
                1.0 - static_cast<double>(decay),
                internal::GetRawOffsetData(running_mean_casted),
                internal::GetRawOffsetData(running_var_casted),
                static_cast<double>(eps),
                internal::GetRawOffsetData(x_mean),
                internal::GetRawOffsetData(x_inv_std));

        // When data type of parameters is converted, say, from fp16
        // to fp32, the values of fp32 arrays of running_mean and
        // running_var updated by batchNormalizationForwardTraining
        // must be explicitly written back to their original fp16 arrays.
        UpdateRunning(running_mean, running_mean_casted);
        UpdateRunning(running_var, running_var_casted);

        if (state.has_value()) {
            state->reset(new CudaBatchNormState{std::move(x_cont), std::move(x_mean), std::move(x_inv_std)});
        }
    }
};

CHAINERX_REGISTER_OP_CUDA(BatchNormForwardOp, CudaBatchNormForwardOp);

class CudaBatchNormBackwardOp : public BatchNormBackwardOp {
public:
    void Call(
            const Array& /*x*/,
            const Array& gamma,
            const Array& gout,
            Scalar eps,
            const Axes& axis,
            const Array& gx,
            const Array& ggamma,
            const Array& gbeta,
            nonstd::optional<std::shared_ptr<void>>& state) override {
        // TODO(hvy): Implement recomputation of x_cont, x_mean and x_inv_std in case they are not given by the state.
        CHAINERX_ASSERT(state.has_value());
        auto state_ptr = reinterpret_cast<CudaBatchNormState*>(state->get());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        const Array& x_cont = state_ptr->x_cont;
        const Array& x_mean = state_ptr->x_mean;
        const Array& x_inv_std = state_ptr->x_inv_std;

        if (CHAINERX_DEBUG) {
            Shape reduced_shape = internal::ReduceShape(x_cont.shape(), axis, true);
            CHAINERX_ASSERT(reduced_shape == gamma.shape());
            CHAINERX_ASSERT(x_cont.shape() == gout.shape());

            CHAINERX_ASSERT(internal::GetArrayBody(x_mean) != nullptr);
            CHAINERX_ASSERT(internal::GetArrayBody(x_inv_std) != nullptr);

            CHAINERX_ASSERT(&x_cont.device() == &x_mean.device());
            CHAINERX_ASSERT(&x_cont.device() == &x_inv_std.device());
            CHAINERX_ASSERT(&x_cont.device() == &gamma.device());
            CHAINERX_ASSERT(&x_cont.device() == &gout.device());

            CHAINERX_ASSERT(x_cont.IsContiguous());
        }

        if (static_cast<double>(eps) < CUDNN_BN_MIN_EPSILON) {
            throw CudnnError{"Minimum allowed epsilon is ", CUDNN_BN_MIN_EPSILON, " but found ", eps, "."};
        }

        CudaDevice& device = dynamic_cast<CudaDevice&>(x_cont.device());
        CudaSetDeviceScope scope{device.index()};

        Array gout_cont = internal::AsContiguous(gout);
        Array gx_cont = EmptyLike(x_cont, device);
        cuda_internal::CudnnTensorDescriptor x_desc{x_cont};

        cudnnBatchNormMode_t mode = GetBatchNormMode(axis);

        CudnnBNTensorDescriptor gamma_beta_mean_var_desc{x_desc, mode};
        Dtype gamma_beta_mean_var_dtype = gamma_beta_mean_var_desc.GetDtype();
        Shape gamma_beta_mean_var_shape = internal::ReduceShape(x_cont.shape(), axis, true);

        Array gamma_casted_cont = internal::AsContiguous(gamma, gamma_beta_mean_var_dtype);
        Array ggamma_casted_cont = Empty(gamma_beta_mean_var_shape, gamma_beta_mean_var_dtype, device);
        Array gbeta_casted_cont = Empty(gamma_beta_mean_var_shape, gamma_beta_mean_var_dtype, device);

        CHAINERX_ASSERT(gamma_beta_mean_var_dtype == x_mean.dtype());
        CHAINERX_ASSERT(gamma_beta_mean_var_dtype == x_inv_std.dtype());
        CHAINERX_ASSERT(x_mean.IsContiguous());
        CHAINERX_ASSERT(x_inv_std.IsContiguous());

        Dtype dtype = x_cont.dtype();

        device.cudnn_handle().Call(
                cudnnBatchNormalizationBackward,
                mode,
                cuda_internal::GetCudnnCoefficientPtr<1>(dtype),
                cuda_internal::GetCudnnCoefficientPtr<0>(dtype),
                cuda_internal::GetCudnnCoefficientPtr<1>(dtype),
                cuda_internal::GetCudnnCoefficientPtr<0>(dtype),
                *x_desc,
                internal::GetRawOffsetData(x_cont),
                *x_desc,
                internal::GetRawOffsetData(gout_cont),
                *x_desc,
                internal::GetRawOffsetData(gx_cont),
                *gamma_beta_mean_var_desc,
                internal::GetRawOffsetData(gamma_casted_cont),
                internal::GetRawOffsetData(ggamma_casted_cont),
                internal::GetRawOffsetData(gbeta_casted_cont),
                static_cast<double>(eps),
                internal::GetRawOffsetData(x_mean),
                internal::GetRawOffsetData(x_inv_std));

        // TODO(hvy): Consider writing directly in the routines/ops above.
        device.AsType(gx_cont, gx);
        device.AsType(ggamma_casted_cont, ggamma);
        device.AsType(gbeta_casted_cont, gbeta);
    }
};

CHAINERX_REGISTER_OP_CUDA(BatchNormBackwardOp, CudaBatchNormBackwardOp);

class CudaFixedBatchNormForwardOp : public FixedBatchNormForwardOp {
public:
    void Call(
            const Array& x,
            const Array& gamma,
            const Array& beta,
            const Array& mean,
            const Array& var,
            Scalar eps,
            const Axes& axis,
            const Array& out) override {
        if (CHAINERX_DEBUG) {
            Shape reduced_shape = internal::ReduceShape(x.shape(), axis, true);
            CHAINERX_ASSERT(gamma.shape() == reduced_shape);
            CHAINERX_ASSERT(beta.shape() == reduced_shape);
            CHAINERX_ASSERT(mean.shape() == reduced_shape);
            CHAINERX_ASSERT(var.shape() == reduced_shape);

            CHAINERX_ASSERT(&x.device() == &gamma.device());
            CHAINERX_ASSERT(&x.device() == &beta.device());
            CHAINERX_ASSERT(&x.device() == &mean.device());
            CHAINERX_ASSERT(&x.device() == &var.device());

            CHAINERX_ASSERT(GetKind(x.dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(gamma.dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(beta.dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(mean.dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(var.dtype()) == DtypeKind::kFloat);
        }

        if (static_cast<double>(eps) < CUDNN_BN_MIN_EPSILON) {
            throw CudnnError{"Minimum allowed epsilon is ", CUDNN_BN_MIN_EPSILON, " but found ", eps, "."};
        }

        CudaDevice& device = dynamic_cast<CudaDevice&>(x.device());
        CudaSetDeviceScope scope{device.index()};

        Array x_cont = internal::AsContiguous(x);
        cuda_internal::CudnnTensorDescriptor x_desc{x_cont};

        cudnnBatchNormMode_t mode = GetBatchNormMode(axis);

        CudnnBNTensorDescriptor gamma_beta_mean_var_desc{x_desc, mode};
        Dtype gamma_beta_mean_var_dtype = gamma_beta_mean_var_desc.GetDtype();

        Array gamma_casted_cont = internal::AsContiguous(gamma, gamma_beta_mean_var_dtype);
        Array beta_casted_cont = internal::AsContiguous(beta, gamma_beta_mean_var_dtype);
        Array mean_casted_cont = internal::AsContiguous(mean, gamma_beta_mean_var_dtype);
        Array var_casted_cont = internal::AsContiguous(var, gamma_beta_mean_var_dtype);

        Dtype dtype = x_cont.dtype();

        device.cudnn_handle().Call(
                cudnnBatchNormalizationForwardInference,
                GetBatchNormMode(axis),
                cuda_internal::GetCudnnCoefficientPtr<1>(dtype),
                cuda_internal::GetCudnnCoefficientPtr<0>(dtype),
                *x_desc,
                internal::GetRawOffsetData(x_cont),
                *x_desc,
                internal::GetRawOffsetData(out),
                *gamma_beta_mean_var_desc,
                internal::GetRawOffsetData(gamma_casted_cont),
                internal::GetRawOffsetData(beta_casted_cont),
                internal::GetRawOffsetData(mean_casted_cont),
                internal::GetRawOffsetData(var_casted_cont),
                static_cast<double>(eps));
    }
};

CHAINERX_REGISTER_OP_CUDA(FixedBatchNormForwardOp, CudaFixedBatchNormForwardOp);

}  // namespace cuda
}  // namespace chainerx
