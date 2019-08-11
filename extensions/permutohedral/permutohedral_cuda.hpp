// Copyright 2016 Max Planck Society
// Distributed under the BSD-3 Software license,
// (See accompanying file ../../../../LICENSE.txt or copy at
// https://opensource.org/licenses/BSD-3-Clause)

#ifndef PERMUTOHEDRAL_CUDA_HPP
#define PERMUTOHEDRAL_CUDA_HPP

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <ATen/ATen.h>
#include <cublas_v2.h>
#include <torch/torch.h>
#include <boost/array.hpp>
#include <boost/cstdint.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>

/************************************************/
/***         Some Utility Functions           ***/
/************************************************/

inline int ipow(int base, int exp) {
  int result = 1;
  while (exp) {
    if (exp & 1) result *= base;
    exp >>= 1;
    base *= base;
  }
  return result;
}

inline int get_filter_size(int neighborhood_size, int feature_size) {
  return ipow(neighborhood_size + 1, feature_size + 1) -
         ipow(neighborhood_size, feature_size + 1);
}

inline void advance_in_dimension(const int dimension,
                                 const int increment,
                                 std::vector<boost::int16_t>* key) {
  const int d_ = key->size() - 1;
  for (int k = 0; k <= d_; ++k) {
    (*key)[k] -= increment;
  }
  (*key)[dimension] += increment * (1 + d_);
}

/************************************************/
/***                Hash Table                ***/
/************************************************/

/*! \brief Hash Table for keeping track of lattice occupancy. Taken from
 * Krähenbühl's original DenseCRF lattice code.
 *         (http://www.philkr.net/home/densecrf)
 */

class HashTable {
 private:
  // Don't copy!
  HashTable(const HashTable& o)
    : key_size_(o.key_size_), filled_(0), capacity_(o.capacity_) {
    table_ = new int[capacity_];
    keys_  = new boost::int16_t[(capacity_ / 2 + 10) * key_size_];
    memset(table_, -1, capacity_ * sizeof(int));
  }

  size_t key_size_, filled_, capacity_;
  boost::int16_t* keys_;
  int* table_;
  void grow() {
    // Swap out the old memory
    boost::int16_t* old_keys = keys_;
    int* old_table           = table_;
    int old_capacity         = capacity_;
    capacity_ *= 2;
    // Allocate the new memory
    keys_  = new boost::int16_t[(old_capacity + 10) * key_size_];
    table_ = new int[capacity_];
    memset(table_, -1, capacity_ * sizeof(int));
    memcpy(keys_, old_keys, filled_ * key_size_ * sizeof(boost::int16_t));

    // Reinsert each element
    for (int i = 0; i < old_capacity; i++)
      if (old_table[i] >= 0) {
        int e    = old_table[i];
        size_t h = hash(old_keys + (getKey(e) - keys_)) % capacity_;
        for (; table_[h] >= 0; h = h < capacity_ - 1 ? h + 1 : 0)
          ;
        table_[h] = e;
      }

    delete[] old_keys;
    delete[] old_table;
  }

  size_t hash(const boost::int16_t* k) {
    size_t r = 0;
    for (size_t i = 0; i < key_size_; i++) {
      r += k[i];
      r *= 1664525;
    }
    return r;
  }

 public:
  HashTable(int key_size, int n_elements)
    : key_size_(key_size), filled_(0), capacity_(2 * n_elements) {
    table_ = new int[capacity_];
    keys_  = new boost::int16_t[(capacity_ / 2 + 10) * key_size_];
    memset(table_, -1, capacity_ * sizeof(int));
  }

  ~HashTable() {
    delete[] keys_;
    delete[] table_;
  }

  int size() const { return filled_; }

  void reset() {
    filled_ = 0;
    memset(table_, -1, capacity_ * sizeof(int));
  }

  int find(const boost::int16_t* k, bool create = false) {
    if (2 * filled_ >= capacity_) grow();
    // Get the hash value
    size_t h = hash(k) % capacity_;
    // Find the element with he right key, using linear probing
    while (1) {
      int e = table_[h];
      if (e == -1) {
        if (create) {
          // Insert a new key and return the new id
          for (size_t i = 0; i < key_size_; i++)
            keys_[filled_ * key_size_ + i] = k[i];
          return table_[h] = filled_++;
        } else {
          return -1;
        }
      }
      // Check if the current key is The One
      bool good = true;
      for (size_t i = 0; i < key_size_ && good; i++)
        if (keys_[e * key_size_ + i] != k[i]) good = false;
      if (good) return e;
      // Continue searching
      h++;
      if (h == capacity_) h = 0;
    }
  }

  const boost::int16_t* getKey(int i) const {
    assert(static_cast<int>(i) < filled_);
    return keys_ + i * key_size_;
  }
};

/************************************************/
/***     Permutohedral Lattice Traversal      ***/
/************************************************/

/*! \brief Class functions for traversing the lattice to build neighborhood
 * stucture for convolutions.
 */

class LatticeTraversal {
 public:
  typedef std::vector<boost::int16_t> key_type;

 public:
  explicit LatticeTraversal(int neighborhood_size, int d)
    : neighborhood_size_(neighborhood_size), d_(d) {}

  template <typename TFun>
  void go(const key_type& start_key, TFun yield) const {
    assert(start_key.size() == d_ + 1);

    std::vector<key_type> walking_keys(d_ + 1);
    for (int i = 0; static_cast<int>(i) < walking_keys.size(); ++i) {
      walking_keys[i].resize(start_key.size());
    }

    walk_cuboid(start_key, 0, false, walking_keys, yield);
  }

 private:
  template <typename TFun>
  void walk_cuboid(const key_type& start_key,
                   const int d,
                   const bool has_zero,
                   std::vector<key_type>& walking_keys,
                   TFun yield) const {
    if (d <= d_) {
      key_type& walking_key = walking_keys[d];
      walking_key           = start_key;

      const int range_end = (d < d_ || has_zero) ? neighborhood_size_ + 1 : 1;
      for (int i = 0; i < range_end; ++i) {
        walk_cuboid(walking_key, d + 1, has_zero || i == 0, walking_keys,
                    yield);
        advance_in_dimension(d, 1, &walking_key);
      }
    } else {
      yield(start_key);
    }
  }

  int neighborhood_size_;
  int d_;
};

/************************************************/
/***         Neighborhood Callback            ***/
/************************************************/

/*! \brief Used for approximate lattice traversal.
 */

class NeighborhoodCallback {
 public:
  NeighborhoodCallback(const int step, int* const neighbors, int* n)
    : step_(step), neighbors_(neighbors), n_(*n) {}

  void operator()(const int indx) {
    if (n_ >= 0) neighbors_[n_ * step_] = indx;
    ++n_;
  }

 private:
  const int step_;
  int* const neighbors_;
  int& n_;
};

/************************************************/
/***     Approximate Lattice Traversal        ***/
/************************************************/

/*! \brief Class functions for faster and Approximately
 *         traversing the lattice to build neighborhood stucture for
 * convolutions.
 */

class LatticeApproximateTraversal {
 public:
  LatticeApproximateTraversal(int neighborhood_size,
                              int d,
                              const std::vector<int>& immediate_neighbors,
                              int M)
    : neighborhood_size_(neighborhood_size),
      d_(d),
      immediate_neighbors_(immediate_neighbors),
      M_(M) {}

  template <typename TFun>
  void go(const int start, TFun yield) const {
    walk_approximate(start, 0, false, yield);
  }

 private:
  template <typename TFun>
  void walk_approximate(const int start,
                        const int d,
                        const bool has_zero,
                        TFun yield) const {
    if (d <= d_) {
      int walking = start;

      const int range_end = (d < d_ || has_zero) ? neighborhood_size_ + 1 : 1;
      for (int i = 0; i < range_end; ++i) {
        walk_approximate(walking, d + 1, has_zero || i == 0, yield);
        if (walking >= 0) walking = immediate_neighbors_[walking + M_ * d];
      }
    } else {
      yield(start);
    }
  }

  int neighborhood_size_;
  int d_;
  const std::vector<int>& immediate_neighbors_;
  int M_;
};

/************************************************/
/*** Gaussian Filter for Permutohedral Lattice **/
/************************************************/

/*! \brief Class for high-dimensional Gaussian filter construction.
 *         Useful as 'offset' for the learnable filter.
 */

class GaussianFilter {
 public:
  GaussianFilter(int neighborhood_size, int feature_size)
    : neighborhood_size_(neighborhood_size), feature_size_(feature_size) {
    build_filter();
  }

  const float* filter() { return filter_.data(); }

 private:
  class TraversalCallback {
   public:
    TraversalCallback(HashTable& hash_table) : hash_table_(hash_table) {}

    void operator()(const std::vector<boost::int16_t>& key) {
      hash_table_.find(key.data(), true);
    }

   private:
    HashTable& hash_table_;
  };

  void build_filter() {
    boost::array<float, 2> gauss = {{1, 0.5}};

    const int size = get_filter_size(neighborhood_size_, feature_size_);

    HashTable hash_table(feature_size_, size * (feature_size_ + 1));

    std::vector<float> lattice(size + 1);

    // Insert center of lattice into hash table.
    std::vector<boost::int16_t> center(feature_size_ + 1);
    const int center_index = hash_table.find(center.data(), true) + 1;
    assert(center_index == 1);

    // Insert all other lattice points into the hash table.
    LatticeTraversal traversal(neighborhood_size_, feature_size_);
    TraversalCallback yield(hash_table);
    traversal.go(center, yield);

    // Initialize the center of the lattice.
    lattice[center_index] = 1;

    std::vector<float> tmp_lattice(size + 1);
    std::vector<boost::int16_t> walking_key_up(feature_size_ + 1);
    std::vector<boost::int16_t> walking_key_down(feature_size_ + 1);
    for (int d = 0; d <= feature_size_; ++d) {
      std::fill(tmp_lattice.begin(), tmp_lattice.end(), 0);

      for (int i = 0; i < size; i++) {
        const boost::int16_t* key = hash_table.getKey(i);
        std::copy(key, key + feature_size_ + 1, walking_key_up.begin());
        std::copy(key, key + feature_size_ + 1, walking_key_down.begin());

        float& v = tmp_lattice[i + 1];
        v        = lattice[i + 1] * gauss[0];

        for (int n = 1; n < neighborhood_size_ + 1; ++n) {
          advance_in_dimension(d, 1, &walking_key_up);
          advance_in_dimension(d, -1, &walking_key_down);

          v += (lattice[hash_table.find(walking_key_up.data()) + 1] +
                lattice[hash_table.find(walking_key_down.data()) + 1]) *
               (n < gauss.size() ? gauss[n] : 0);
        }
      }

      lattice.swap(tmp_lattice);
      lattice[0] = 0;
    }

    filter_.resize(size);
    // Normalize the filter according to the center lattice point. Like that we
    // are not creating additional energy for it.
    const float alpha = lattice[1];
    for (int i = 0; i < size; ++i) {
      filter_[i] = lattice[i + 1] / alpha;
    }
  }

  int neighborhood_size_;
  int feature_size_;
  std::vector<float> filter_;
};

/************************************************/
/***          Permutohedral Lattice           ***/
/************************************************/

/*! \brief This is the main class for lattice construction and forward
 * operations in 'learnable' sparse high dimensional filtering. This class
 * defines 'forward' functionatlity. See CPU/GPU specific
 *         'PermutohedralReverseCpu' and 'PermutohedralReverseGpu' for the
 * definition of 'splat', 'blur', 'max' and 'slice' forward functions and the
 * respective backward functions.
 *
 *  'Filter' weights are generic non-seperable high-dimensional permutohedral
 * filters. This class has both CPU and GPU functionality except for the lattice
 * construction, At present, there is no dedicated GPU functions for
 * permutohedral lattice construction.
 *
 *  Some parts of the code are adapted and heavily modified from the separable
 * filter code from Adams et al. 2010
 * (http://graphics.stanford.edu/papers/permutohedral/).
 */

class PermutohedralReverse;

class Permutohedral {
 public:
  typedef GaussianFilter gauss_type;
  struct Lattice {
    int N_, d_;
    int neighborhood_size_;
    int M_;
    std::vector<float> barycentric_;
    std::vector<int> offset_;
    std::vector<int> blur_neighbors_;
  };

 private:
  Permutohedral(const Permutohedral& rhs);
  boost::shared_ptr<const Lattice> lattice_;
  bool check_unique_neighbors(const int* neighbors);
  static void map_back(const std::vector<boost::int16_t>& key, float* const x);

 public:
  Permutohedral();
  static int get_filter_size(int neighborhood_size, int feature_size);
  void init(const float* feature,
            int data_count,
            int feature_size,
            int neighborhood_size,
            bool do_visualization);
  boost::shared_ptr<PermutohedralReverse> compute(const cublasHandle_t& handle,
                                                  const float* filter,
                                                  const float* in,
                                                  int num_output,
                                                  int group,
                                                  int value_size,
                                                  bool do_skip_blur,
                                                  int in_offset,
                                                  int out_offset,
                                                  int in_size,
                                                  int out_size,
                                                  float* out) const;
  boost::shared_ptr<PermutohedralReverse> max_compute(const float* filter,
                                                      const float* in,
                                                      int value_size,
                                                      int in_offset,
                                                      int out_offset,
                                                      int in_size,
                                                      int out_size,
                                                      float* out);
};

/************************************************/
/***          Permutohedral Reverse           ***/
/************************************************/

/*! \brief This is the main class for reverse operations in
 *         'learnable' sparse high dimensional filtering.
 *         This class defines 'backward' functionatlity. See CPU/GPU specific
 *         'PermutohedralReverseCpu' and 'PermutohedralReverseGpu' for the
 * definition of 'splat_tick', 'blur_tick', 'max_tick' and 'slice_tick' backward
 * functions and the respective foward functions.
 *
 */

class PermutohedralReverse {
 public:
  void reverse(const cublasHandle_t& handle,
               const float* diff_in,
               float* diff_out_filter,
               float* diff_out_in);
  void max_reverse(const float* diff_in, float* diff_out_in);

 private:
  // don't copy
  PermutohedralReverse(const PermutohedralReverse& rhs);
  // Only Permutohedral initializes this.
  PermutohedralReverse();
  void init(
    const float* filter,
    int num_output,
    int group,
    int value_size,
    bool do_skip_blur,
    int in_offset,
    int out_offset,
    int in_size,
    int out_size,
    const boost::shared_ptr<const typename Permutohedral::Lattice> lattice);
  void compute(const cublasHandle_t& handle, const float* in, float* out);
  void max_compute(const float* in, float* out);
  void slice(const at::Tensor& data, float* sliced) const;
  void blur(const cublasHandle_t& handle,
            const at::Tensor& splatted,
            const at::Tensor& filter,
            at::Tensor* blurred) const;
  void max(const at::Tensor& splatted, at::Tensor* maxxed);
  void splat(const float* in, at::Tensor* splatted) const;
  static void im2col(const float* im,
                     const int value_size,
                     const int filter_size,
                     const int M,
                     const int start,
                     const int end,
                     const int* blur_neighbors,
                     float* col);
  static void col2im(const float* col,
                     const int value_size,
                     const int filter_size,
                     const int M,
                     const int start,
                     const int end,
                     const int* blur_neighbors,
                     float* im);
  void slice_tick(const float* sliced_tick, at::Tensor* sliced_out) const;
  void blur_tick(const cublasHandle_t& handle,
                 const at::Tensor& blurred_tick,
                 at::Tensor* blurred_out,
                 float* filter_out);
  void max_tick(const at::Tensor& maxxed_tick, at::Tensor* maxxed_out);
  void splat_tick(const at::Tensor& splatted_tick, float* splatted_out);

  at::Tensor filter_;    // Blob<T> filter_;
  at::Tensor splatted_;  // Blob<T> splatted_;

  int d_, N_;
  int neighborhood_size_;
  int M_;

  at::Tensor max_idx_;         // Blob<int> max_idx_;
  at::Tensor barycentric_;     // Blob<T> barycentric_;
  at::Tensor offset_;          // Blob<int> offset_;
  at::Tensor blur_neighbors_;  // Blob<int> blur_neighbors_;

  int in_offset_, out_offset_, in_size_, out_size_;
  int num_output_, group_;
  int value_size_;

  bool do_skip_blur_;

  friend class Permutohedral;
};

#endif /* PERMUTOHEDRAL_CUDA_HPP */