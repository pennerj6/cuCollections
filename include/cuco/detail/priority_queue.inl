#pragma once
#include <cmath>

#include <cuco/detail/priority_queue_kernels.cuh>
#include <cuco/detail/error.hpp>

namespace cuco {

template <typename T, typename Compare, typename Allocator>
priority_queue<T, Compare, Allocator>::priority_queue
                                               (size_t initial_capacity,
                                                size_t node_size,
						Allocator const& allocator) :
	                                        allocator_{allocator},
					        int_allocator_{allocator},
					        t_allocator_{allocator},
					        size_t_allocator_{allocator} {

  node_size_ = node_size;
  
  // Round up to the nearest multiple of node size
  int nodes = ((initial_capacity + node_size_ - 1) / node_size_);

  node_capacity_ = nodes;
  lowest_level_start_ = 1 << (int)log2(nodes);

  // Allocate device variables

  d_size_ = std::allocator_traits<int_allocator_type>::allocate(int_allocator_,
		                                                1);

  CUCO_CUDA_TRY(cudaMemset(d_size_, 0, sizeof(int)));

  d_p_buffer_size_ = std::allocator_traits<size_t_allocator_type>::allocate(
		                                              size_t_allocator_,
							      1);

  CUCO_CUDA_TRY(cudaMemset(d_p_buffer_size_, 0, sizeof(size_t)));

  d_heap_ = std::allocator_traits<t_allocator_type>::allocate(t_allocator_,
		                       node_capacity_ * node_size_ + node_size_);

  d_locks_ = std::allocator_traits<int_allocator_type>::allocate(int_allocator_,
		                                             node_capacity_ + 1);

  CUCO_CUDA_TRY(cudaMemset(d_locks_, 0,
                          sizeof(int) * (node_capacity_ + 1)));


}

template <typename T, typename Compare, typename Allocator>
priority_queue<T, Compare, Allocator>::~priority_queue() {
  std::allocator_traits<int_allocator_type>::deallocate(int_allocator_,
		                                        d_size_, 1);
  std::allocator_traits<size_t_allocator_type>::deallocate(size_t_allocator_,
		                                        d_p_buffer_size_, 1);
  std::allocator_traits<t_allocator_type>::deallocate(t_allocator_,
		                                         d_heap_,
			        node_capacity_ * node_size_ + node_size_);
  std::allocator_traits<int_allocator_type>::deallocate(int_allocator_,
		                                        d_locks_,
				                    node_capacity_ + 1);
}


template <typename T, typename Compare, typename Allocator>
template <typename InputIt>
void priority_queue<T, Compare, Allocator>::push(InputIt first,
                                           InputIt last,
					   cudaStream_t stream,
                                           int block_size,
                                           int grid_size,
                                           bool warp_level) {

  const int kBlockSize = min(block_size, (int)node_size_);
  const int kNumBlocks = grid_size;

  //if (!warp_level) {
    PushKernel<<<kNumBlocks, kBlockSize,
                 get_shmem_size(kBlockSize), stream>>>
              (first, last - first, d_heap_, d_size_,
               node_size_, d_locks_, d_p_buffer_size_, lowest_level_start_,
	       compare_);
  //} else {
  //  PushKernelWarp<<<kNumBlocks, kBlockSize,
  //               get_shmem_size(32) * kBlockSize / 32, stream>>>
  //            (first, last - first, d_heap_, d_size_,
  //             node_size_, d_locks_, d_p_buffer_size_,
  //             lowest_level_start_, get_shmem_size(32), compare_);
  //}

  CUCO_CUDA_TRY(cudaGetLastError());
}

template <typename T, typename Compare, typename Allocator>
template <typename OutputIt>
void priority_queue<T, Compare, Allocator>::pop(OutputIt first,
                                          OutputIt last,
					  cudaStream_t stream,
                                          int block_size,
                                          int grid_size,
                                          bool warp_level) {
  
  const int kBlockSize = min(block_size, (int)node_size_);
  const int kNumBlocks = grid_size;

  auto pop_size = last - first;
  const auto partial = pop_size % node_size_;

  if (partial != 0) {
    PopPartialNodeKernel<<<1, kBlockSize, get_shmem_size(kBlockSize),
	                   stream>>>
                        (first, partial, d_heap_, d_size_,
			 node_size_, d_locks_, d_p_buffer_size_,
                         lowest_level_start_, node_capacity_, compare_);
  }

  pop_size -= partial;
  first += partial;
   

  //if (!warp_level) {
  PopKernel<<<kNumBlocks, kBlockSize,
                 get_shmem_size(kBlockSize), stream>>>
             (first, pop_size, d_heap_, d_size_,
              node_size_, d_locks_, d_p_buffer_size_,
              lowest_level_start_, node_capacity_, compare_);
  //} else {
  //  PopKernelWarp<<<kNumBlocks, kBlockSize,
  //               get_shmem_size(32) * kBlockSize / 32, stream>>>
  //           (first, last - first, d_heap_, d_size_,
  //            node_size_, d_locks_, d_p_buffer_size_,
  //            lowest_level_start_,
  //            node_capacity_, get_shmem_size(32), compare_);

  //}

  CUCO_CUDA_TRY(cudaGetLastError());
}

template <typename T, typename Compare, typename Allocator>
template <typename CG, typename InputIt>
__device__ void priority_queue<T, Compare, Allocator>
                                 ::device_mutable_view::push(
                                                  CG const& g,
                                                  InputIt first,
                                                  InputIt last,
                                                  void *temp_storage) {

  SharedMemoryLayout<T> shmem =
       GetSharedMemoryLayout<T>((int*)temp_storage,
                                         g.size(), node_size_);
  if (last - first == node_size_) {
    PushSingleNode(g, first, d_heap_, d_size_, node_size_,
                   d_locks_, lowest_level_start_, shmem, compare_);
  } else if (last - first < node_size_) {
    PushPartialNode(g, first, last - first, d_heap_,
                         d_size_, node_size_, d_locks_,
                         d_p_buffer_size_, lowest_level_start_, shmem,
			 compare_);
  }
}

template <typename T, typename Compare, typename Allocator>
template <typename CG, typename OutputIt>
__device__ void priority_queue<T, Compare, Allocator>
                                       ::device_mutable_view::pop(
                                                      CG const& g,
                                                      OutputIt first,
                                                      OutputIt last,
                                                      void *temp_storage) {
  SharedMemoryLayout<T> shmem =
       GetSharedMemoryLayout<T>((int*)temp_storage,
                                         g.size(), node_size_);

  if (last - first == node_size_) {
    PopSingleNode(g, first, d_heap_, d_size_, node_size_, d_locks_,
                  d_p_buffer_size_, lowest_level_start_,
                  node_capacity_, shmem, compare_);
  } else {
    PopPartialNode(g, first, last - first, d_heap_, d_size_, node_size_,
                   d_locks_, d_p_buffer_size_, lowest_level_start_,
                   node_capacity_, shmem, compare_);
  }
}

}
