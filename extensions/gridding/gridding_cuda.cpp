/*
 * @Author: Haozhe Xie
 * @Date:   2019-11-13 10:52:53
 * @Last Modified by:   Haozhe Xie
 * @Last Modified time: 2019-11-19 21:05:57
 * @Email:  cshzxie@gmail.com
 */

#include <ATen/cuda/CUDAContext.h>
#include <torch/extension.h>

// NOTE: AT_ASSERT has become AT_CHECK on master after 0.4.
#define CHECK_CUDA(x) \
  AT_ASSERTM(x.type().is_cuda(), #x " must be a CUDA tensor")
#define CHECK_CONTIGUOUS(x) \
  AT_ASSERTM(x.is_contiguous(), #x " must be contiguous")
#define CHECK_INPUT(x) \
  CHECK_CUDA(x);       \
  CHECK_CONTIGUOUS(x)

torch::Tensor gridding_cuda_forward(float min_x,
                                    float max_x,
                                    float min_y,
                                    float max_y,
                                    float min_z,
                                    float max_z,
                                    torch::Tensor ptcloud,
                                    cudaStream_t stream);

torch::Tensor gridding_forward(float min_x,
                               float max_x,
                               float min_y,
                               float max_y,
                               float min_z,
                               float max_z,
                               torch::Tensor ptcloud) {
  CHECK_INPUT(ptcloud);

  cudaStream_t stream = at::cuda::getCurrentCUDAStream();
  return gridding_cuda_forward(min_x, max_x, min_y, max_y, min_z, max_z, ptcloud, stream);
}

torch::Tensor gridding_backward(torch::Tensor min_max_values,
                                torch::Tensor ptcloud) {}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("forward", &gridding_forward, "Gridding Distance forward (CUDA)");
  m.def("backward", &gridding_backward, "Gridding Distance backward (CUDA)");
}