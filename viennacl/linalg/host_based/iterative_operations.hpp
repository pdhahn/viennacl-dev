#ifndef VIENNACL_LINALG_HOST_BASED_ITERATIVE_OPERATIONS_HPP_
#define VIENNACL_LINALG_HOST_BASED_ITERATIVE_OPERATIONS_HPP_

/* =========================================================================
   Copyright (c) 2010-2014, Institute for Microelectronics,
                            Institute for Analysis and Scientific Computing,
                            TU Wien.
   Portions of this software are copyright by UChicago Argonne, LLC.

                            -----------------
                  ViennaCL - The Vienna Computing Library
                            -----------------

   Project Head:    Karl Rupp                   rupp@iue.tuwien.ac.at

   (A list of authors and contributors can be found in the PDF manual)

   License:         MIT (X11), see file LICENSE in the base directory
============================================================================= */

/** @file viennacl/linalg/host_based/iterative_operations.hpp
    @brief Implementations of specialized kernels for fast iterative solvers using OpenMP on the CPU
*/

#include <cmath>
#include <algorithm>  //for std::max and std::min

#include "viennacl/forwards.h"
#include "viennacl/scalar.hpp"
#include "viennacl/tools/tools.hpp"
#include "viennacl/meta/predicate.hpp"
#include "viennacl/meta/enable_if.hpp"
#include "viennacl/traits/size.hpp"
#include "viennacl/traits/start.hpp"
#include "viennacl/linalg/host_based/common.hpp"
#include "viennacl/linalg/detail/op_applier.hpp"
#include "viennacl/traits/stride.hpp"


// Minimum vector size for using OpenMP on vector operations:
#ifndef VIENNACL_OPENMP_VECTOR_MIN_SIZE
  #define VIENNACL_OPENMP_VECTOR_MIN_SIZE  5000
#endif

namespace viennacl
{
  namespace linalg
  {
    namespace host_based
    {
      /** @brief Performs a joint vector update operation needed for an efficient pipelined CG algorithm.
        *
        * This routines computes for vectors 'result', 'p', 'r', 'Ap':
        *   result += alpha * p;
        *   r      -= alpha * Ap;
        *   p       = r + beta * p;
        * and runs the parallel reduction stage for computing inner_prod(r,r)
        */
      template <typename T>
      void pipelined_cg_vector_update(vector_base<T> & result,
                                      T alpha,
                                      vector_base<T> & p,
                                      vector_base<T> & r,
                                      vector_base<T> const & Ap,
                                      T beta,
                                      vector_base<T> & inner_prod_buffer)
      {
        typedef T        value_type;

        value_type       * data_result = detail::extract_raw_pointer<value_type>(result);
        value_type       * data_p      = detail::extract_raw_pointer<value_type>(p);
        value_type       * data_r      = detail::extract_raw_pointer<value_type>(r);
        value_type const * data_Ap     = detail::extract_raw_pointer<value_type>(Ap);
        value_type       * data_buffer = detail::extract_raw_pointer<value_type>(inner_prod_buffer);

        // Note: Due to the special setting in CG, there is no need to check for sizes and strides
        vcl_size_t size  = viennacl::traits::size(result);

        value_type inner_prod_r = 0;
        for (long i = 0; i < static_cast<long>(size); ++i)
        {
          value_type value_p = data_p[static_cast<vcl_size_t>(i)];
          value_type value_r = data_r[static_cast<vcl_size_t>(i)];


          data_result[static_cast<vcl_size_t>(i)] += alpha * value_p;
          value_r -= alpha * data_Ap[static_cast<vcl_size_t>(i)];
          value_p  = value_r + beta * value_p;
          inner_prod_r += value_r * value_r;

          data_p[static_cast<vcl_size_t>(i)] = value_p;
          data_r[static_cast<vcl_size_t>(i)] = value_r;
        }

        data_buffer[0] = inner_prod_r;
      }


      /** @brief Performs a fused matrix-vector product with a compressed_matrix for an efficient pipelined CG algorithm.
        *
        * This routines computes for a matrix A and vectors 'p' and 'Ap':
        *   Ap = prod(A, p);
        * and computes the two reduction stages for computing inner_prod(p,Ap), inner_prod(Ap,Ap)
        */
      template <typename T>
      void pipelined_cg_prod(compressed_matrix<T> const & A,
                             vector_base<T> const & p,
                             vector_base<T> & Ap,
                             vector_base<T> & inner_prod_buffer)
      {
        typedef T        value_type;

        value_type         * Ap_buf      = detail::extract_raw_pointer<value_type>(Ap.handle());
        value_type   const *  p_buf      = detail::extract_raw_pointer<value_type>(p.handle());
        value_type   const * elements    = detail::extract_raw_pointer<value_type>(A.handle());
        unsigned int const *  row_buffer = detail::extract_raw_pointer<unsigned int>(A.handle1());
        unsigned int const *  col_buffer = detail::extract_raw_pointer<unsigned int>(A.handle2());
        value_type         * data_buffer = detail::extract_raw_pointer<value_type>(inner_prod_buffer);

        std::size_t buffer_size_per_vector = inner_prod_buffer.size() / 3;


        value_type inner_prod_ApAp = 0;
        value_type inner_prod_pAp = 0;
        for (long row = 0; row < static_cast<long>(A.size1()); ++row)
        {
          value_type dot_prod = 0;
          value_type val_p_diag = p_buf[static_cast<vcl_size_t>(row)]; //likely to be loaded from cache if required again in this row

          vcl_size_t row_end = row_buffer[row+1];
          for (vcl_size_t i = row_buffer[row]; i < row_end; ++i)
            dot_prod += elements[i] * p_buf[col_buffer[i]];

          // update contributions for the inner products (Ap, Ap) and (p, Ap)
          Ap_buf[static_cast<vcl_size_t>(row)] = dot_prod;
          inner_prod_ApAp += dot_prod * dot_prod;
          inner_prod_pAp  += val_p_diag * dot_prod;
        }

        data_buffer[    buffer_size_per_vector] = inner_prod_ApAp;
        data_buffer[2 * buffer_size_per_vector] = inner_prod_pAp;
      }



      /** @brief Performs a fused matrix-vector product with a coordinate_matrix for an efficient pipelined CG algorithm.
        *
        * This routines computes for a matrix A and vectors 'p' and 'Ap':
        *   Ap = prod(A, p);
        * and computes the two reduction stages for computing inner_prod(p,Ap), inner_prod(Ap,Ap)
        */
      template <typename T>
      void pipelined_cg_prod(coordinate_matrix<T> const & A,
                             vector_base<T> const & p,
                             vector_base<T> & Ap,
                             vector_base<T> & inner_prod_buffer)
      {
        typedef T        value_type;

        value_type         * Ap_buf       = detail::extract_raw_pointer<value_type>(Ap.handle());
        value_type   const *  p_buf       = detail::extract_raw_pointer<value_type>(p.handle());
        value_type   const * elements     = detail::extract_raw_pointer<value_type>(A.handle());
        unsigned int const * coord_buffer = detail::extract_raw_pointer<unsigned int>(A.handle12());
        value_type         * data_buffer  = detail::extract_raw_pointer<value_type>(inner_prod_buffer);

        std::size_t buffer_size_per_vector = inner_prod_buffer.size() / 3;

        // flush result buffer (cannot be expected to be zero)
        for (vcl_size_t i = 0; i< Ap.size(); ++i)
          Ap_buf[i] = 0;

        // matrix-vector product with a general COO format
        for (vcl_size_t i = 0; i < A.nnz(); ++i)
          Ap_buf[coord_buffer[2*i]] += elements[i] * p_buf[coord_buffer[2*i+1]];

        // computing the inner products (Ap, Ap) and (p, Ap):
        // Note: The COO format does not allow to inject the subsequent operations into the matrix-vector product, because row and column ordering assumptions are too weak
        value_type inner_prod_ApAp = 0;
        value_type inner_prod_pAp = 0;
        for (vcl_size_t i = 0; i<Ap.size(); ++i)
        {
          T value_Ap = Ap_buf[i];
          T value_p  =  p_buf[i];

          inner_prod_ApAp += value_Ap * value_Ap;
          inner_prod_pAp  += value_Ap * value_p;
        }

        data_buffer[    buffer_size_per_vector] = inner_prod_ApAp;
        data_buffer[2 * buffer_size_per_vector] = inner_prod_pAp;
      }


      /** @brief Performs a fused matrix-vector product with an ell_matrix for an efficient pipelined CG algorithm.
        *
        * This routines computes for a matrix A and vectors 'p' and 'Ap':
        *   Ap = prod(A, p);
        * and computes the two reduction stages for computing inner_prod(p,Ap), inner_prod(Ap,Ap)
        */
      template <typename T>
      void pipelined_cg_prod(ell_matrix<T> const & A,
                             vector_base<T> const & p,
                             vector_base<T> & Ap,
                             vector_base<T> & inner_prod_buffer)
      {
        typedef T        value_type;

        value_type         * Ap_buf       = detail::extract_raw_pointer<value_type>(Ap.handle());
        value_type   const *  p_buf       = detail::extract_raw_pointer<value_type>(p.handle());
        value_type   const * elements     = detail::extract_raw_pointer<value_type>(A.handle());
        unsigned int const * coords       = detail::extract_raw_pointer<unsigned int>(A.handle2());
        value_type         * data_buffer  = detail::extract_raw_pointer<value_type>(inner_prod_buffer);

        std::size_t buffer_size_per_vector = inner_prod_buffer.size() / 3;

        value_type inner_prod_ApAp = 0;
        value_type inner_prod_pAp = 0;
        for(vcl_size_t row = 0; row < A.size1(); ++row)
        {
          value_type sum = 0;
          value_type val_p_diag = p_buf[static_cast<vcl_size_t>(row)]; //likely to be loaded from cache if required again in this row

          for(unsigned int item_id = 0; item_id < A.internal_maxnnz(); ++item_id)
          {
            vcl_size_t offset = row + item_id * A.internal_size1();
            value_type val = elements[offset];

            if (val)
              sum += (p_buf[coords[offset]] * val);
          }

          Ap_buf[row] = sum;
          inner_prod_ApAp += sum * sum;
          inner_prod_pAp  += val_p_diag * sum;
        }

        data_buffer[    buffer_size_per_vector] = inner_prod_ApAp;
        data_buffer[2 * buffer_size_per_vector] = inner_prod_pAp;
      }


      /** @brief Performs a fused matrix-vector product with an sliced_ell_matrix for an efficient pipelined CG algorithm.
        *
        * This routines computes for a matrix A and vectors 'p' and 'Ap':
        *   Ap = prod(A, p);
        * and computes the two reduction stages for computing inner_prod(p,Ap), inner_prod(Ap,Ap)
        */
      template <typename T, typename IndexT>
      void pipelined_cg_prod(sliced_ell_matrix<T, IndexT> const & A,
                             vector_base<T> const & p,
                             vector_base<T> & Ap,
                             vector_base<T> & inner_prod_buffer)
      {
        typedef T        value_type;

        value_type       * Ap_buf            = detail::extract_raw_pointer<value_type>(Ap.handle());
        value_type const *  p_buf            = detail::extract_raw_pointer<value_type>(p.handle());
        value_type const * elements          = detail::extract_raw_pointer<value_type>(A.handle());
        IndexT     const * columns_per_block = detail::extract_raw_pointer<IndexT>(A.handle1());
        IndexT     const * column_indices    = detail::extract_raw_pointer<IndexT>(A.handle2());
        IndexT     const * block_start       = detail::extract_raw_pointer<IndexT>(A.handle3());
        value_type         * data_buffer     = detail::extract_raw_pointer<value_type>(inner_prod_buffer);

        std::size_t buffer_size_per_vector = inner_prod_buffer.size() / 3;

        vcl_size_t num_blocks = A.size1() / A.rows_per_block() + 1;
        std::vector<value_type> result_values(A.rows_per_block());

        value_type inner_prod_ApAp = 0;
        value_type inner_prod_pAp = 0;
        for (vcl_size_t block_idx = 0; block_idx < num_blocks; ++block_idx)
        {
          vcl_size_t current_columns_per_block = columns_per_block[block_idx];

          for (vcl_size_t i=0; i<result_values.size(); ++i)
            result_values[i] = 0;

          for (IndexT column_entry_index = 0;
                      column_entry_index < current_columns_per_block;
                    ++column_entry_index)
          {
            vcl_size_t stride_start = block_start[block_idx] + column_entry_index * A.rows_per_block();
            // Note: This for-loop may be unrolled by hand for exploiting vectorization
            //       Careful benchmarking recommended first, memory channels may be saturated already!
            for(IndexT row_in_block = 0; row_in_block < A.rows_per_block(); ++row_in_block)
            {
              value_type val = elements[stride_start + row_in_block];

              result_values[row_in_block] += val ? p_buf[column_indices[stride_start + row_in_block]] * val : 0;
            }
          }

          vcl_size_t first_row_in_matrix = block_idx * A.rows_per_block();
          for(IndexT row_in_block = 0; row_in_block < A.rows_per_block(); ++row_in_block)
          {
            vcl_size_t row = first_row_in_matrix + row_in_block;
            if (row < Ap.size())
            {
              value_type row_result = result_values[row_in_block];

              Ap_buf[row] = row_result;
              inner_prod_ApAp += row_result * row_result;
              inner_prod_pAp  += p_buf[row] * row_result;
            }
          }
        }

        data_buffer[    buffer_size_per_vector] = inner_prod_ApAp;
        data_buffer[2 * buffer_size_per_vector] = inner_prod_pAp;
      }




      /** @brief Performs a fused matrix-vector product with an hyb_matrix for an efficient pipelined CG algorithm.
        *
        * This routines computes for a matrix A and vectors 'p' and 'Ap':
        *   Ap = prod(A, p);
        * and computes the two reduction stages for computing inner_prod(p,Ap), inner_prod(Ap,Ap)
        */
      template <typename T>
      void pipelined_cg_prod(hyb_matrix<T> const & A,
                             vector_base<T> const & p,
                             vector_base<T> & Ap,
                             vector_base<T> & inner_prod_buffer)
      {
        typedef T            value_type;
        typedef unsigned int index_type;

        value_type       * Ap_buf            = detail::extract_raw_pointer<value_type>(Ap.handle());
        value_type const *  p_buf            = detail::extract_raw_pointer<value_type>(p.handle());
        value_type const * elements          = detail::extract_raw_pointer<value_type>(A.handle());
        index_type const * coords            = detail::extract_raw_pointer<index_type>(A.handle2());
        value_type const * csr_elements      = detail::extract_raw_pointer<value_type>(A.handle5());
        index_type const * csr_row_buffer    = detail::extract_raw_pointer<index_type>(A.handle3());
        index_type const * csr_col_buffer    = detail::extract_raw_pointer<index_type>(A.handle4());
        value_type         * data_buffer     = detail::extract_raw_pointer<value_type>(inner_prod_buffer);

        std::size_t buffer_size_per_vector = inner_prod_buffer.size() / 3;

        value_type inner_prod_ApAp = 0;
        value_type inner_prod_pAp = 0;
        for(vcl_size_t row = 0; row < A.size1(); ++row)
        {
          value_type val_p_diag = p_buf[static_cast<vcl_size_t>(row)]; //likely to be loaded from cache if required again in this row
          value_type sum = 0;

          //
          // Part 1: Process ELL part
          //
          for(index_type item_id = 0; item_id < A.internal_ellnnz(); ++item_id)
          {
            vcl_size_t offset = row + item_id * A.internal_size1();
            value_type val = elements[offset];

            if (val)
              sum += p_buf[coords[offset]] * val;
          }

          //
          // Part 2: Process HYB part
          //
          vcl_size_t col_begin = csr_row_buffer[row];
          vcl_size_t col_end   = csr_row_buffer[row + 1];

          for(vcl_size_t item_id = col_begin; item_id < col_end; item_id++)
            sum += p_buf[csr_col_buffer[item_id]] * csr_elements[item_id];

          Ap_buf[row] = sum;
          inner_prod_ApAp += sum * sum;
          inner_prod_pAp  += val_p_diag * sum;
        }

        data_buffer[    buffer_size_per_vector] = inner_prod_ApAp;
        data_buffer[2 * buffer_size_per_vector] = inner_prod_pAp;
      }



    } //namespace host_based
  } //namespace linalg
} //namespace viennacl


#endif
