/*
 * Copyright (c) IBM Corporation 2020.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *    3. Neither the name of the OpenBLAS project nor the names of
 *       its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "common.h"
#include <vecintrin.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef COMPLEX
#error "Handling for complex numbers is not supported in this kernel"
#endif

#ifdef DOUBLE
#define UNROLL_M DGEMM_DEFAULT_UNROLL_M
#define UNROLL_N DGEMM_DEFAULT_UNROLL_N
#else
#define UNROLL_M SGEMM_DEFAULT_UNROLL_M
#define UNROLL_N SGEMM_DEFAULT_UNROLL_N
#endif

static const size_t unroll_m = UNROLL_M;
static const size_t unroll_n = UNROLL_N;

/*
 * Background:
 *
 * The algorithm of GotoBLAS / OpenBLAS breaks down the matrix multiplication
 * problem by splitting all matrices into partitions multiple times, so that the
 * submatrices fit into the L1 or L2 caches. As a result, each multiplication of
 * submatrices can stream data fast from L1 and L2 caches. Inbetween, it copies
 * and rearranges the submatrices to enable contiguous memory accesses to
 * improve locality in both caches and TLBs.
 *
 * At the heart of the algorithm is this kernel, which multiplies, a "Block
 * matrix" A (small dimensions) with a "Panel matrix" B (number of rows is
 * small) and adds the result into a "Panel matrix" C; GotoBLAS calls this
 * operation GEBP. This kernel further partitions GEBP twice, such that (1)
 * submatrices of C and B fit into the L1 caches (GEBP_column_block) and (2) a
 * block of C fits into the registers, while multiplying panels from A and B
 * streamed from the L2 and L1 cache, respectively (GEBP_block).
 *
 *
 * Algorithm GEBP(A, B, C, m, n, k, alpha):
 *
 * The problem is calculating C += alpha * (A * B)
 * C is an m x n matrix, A is an m x k matrix, B is an k x n matrix.
 *
 * - C is in column-major-order, with an offset of ldc to the element in the
 *   next column (same row).
 * - A is in row-major-order yet stores SGEMM_UNROLL_M elements of each column
 *   contiguously while walking along rows.
 * - B is in column-major-order but packs SGEMM_UNROLL_N elements of a row
 *   contiguously.
 * If the numbers of rows and columns are not multiples of SGEMM_UNROLL_M or
 * SGEMM_UNROLL_N, the remaining elements are arranged in blocks with power-of-2
 * dimensions (e.g., 5 remaining columns would be in a block-of-4 and a
 * block-of-1).
 *
 * Note that packing A and B into that form is taken care of by the caller in
 * driver/level3/level3.c (actually done by "copy kernels").
 *
 * Steps:
 * - Partition C and B into blocks of n_r (SGEMM_UNROLL_N) columns, C_j and B_j.
 *   Now, B_j should fit into the L1 cache.
 * - For each partition, calculate C_j += alpha * (A * B_j) by
 *     (1) Calculate C_aux := A * B_j (see below)
 *     (2) unpack C_j = C_j + alpha * C_aux
 *
 *
 * Algorithm for Calculating C_aux:
 *
 * - Further partition C_aux and A into groups of m_r (SGEMM_UNROLL_M) rows,
 *   such that the m_r x n_r-submatrix of C_aux can be held in registers. Each
 *   submatrix of C_aux can be calculated independently, and the registers are
 *   added back into C_j.
 *
 * - For each row-block of C_aux:
 *   (uses a row block of A and full B_j)
 *    - stream over all columns of A, multiply with elements from B and
 *      accumulate in registers. (use different inner-kernels to exploit
 *      vectorization for varying block sizes)
 *    - add alpha * row block of C_aux back into C_j.
 *
 * Reference:
 *
 * The summary above is based on staring at various kernel implementations and:
 * K. Goto and R. A. Van de Geijn, Anatomy of High-Performance Matrix
 * Multiplication, in ACM Transactions of Mathematical Software, Vol.  34, No.
 * 3, May 2008.
 */

#define VLEN_BYTES 16
#define VLEN_FLOATS (VLEN_BYTES / sizeof(FLOAT))

typedef FLOAT vector_float __attribute__ ((vector_size (16)));

/**
 * Calculate for a row-block in C_i of size ROWSxCOLS using vector intrinsics.
 *
 * @param[in] 	A	Pointer current block of input matrix A.
 * @param[in]	k	Number of columns in A.
 * @param[in]	B	Pointer current block of input matrix B.
 * @param[inout] C	Pointer current block of output matrix C.
 * @param[in]	ldc	Offset between elements in adjacent columns in C.
 * @param[in]	alpha	Scalar factor.
 */
#define VECTOR_BLOCK(ROWS, COLS)                                              \
	static inline void GEBP_block_##ROWS##_##COLS(                        \
	    FLOAT const *restrict A, BLASLONG bk, FLOAT const *restrict B,    \
	    FLOAT *restrict C, BLASLONG ldc, FLOAT alpha) {                   \
		_Static_assert(                                               \
		    ROWS % VLEN_FLOATS == 0,                                  \
		    "rows in block must be multiples of vector length");      \
		vector_float Caux[ROWS / VLEN_FLOATS][COLS];                  \
                                                                              \
		for (BLASLONG i = 0; i < ROWS / VLEN_FLOATS; i++)             \
			for (BLASLONG j = 0; j < COLS; j++)                   \
				Caux[i][j] = vec_splats(ZERO);                \
                                                                              \
		/*                                                            \
		 * Stream over the row-block of A, which is packed            \
		 * column-by-column, multiply by coefficients in B and add up \
		 * into temporaries Caux (which the compiler will hold in     \
		 * registers). Vectorization: Multiply column vectors from A  \
		 * with scalars from B and add up in column vectors of Caux.  \
		 * That equates to unrolling the loop over rows (in i) and    \
		 * executing each unrolled iteration as a vector element.     \
		 */                                                           \
		for (BLASLONG k = 0; k < bk; k++) {                           \
			for (BLASLONG i = 0; i < ROWS / VLEN_FLOATS; i++) {   \
				vector_float Ak =                             \
				    *(vector_float *)(A + i * VLEN_FLOATS +   \
						      k * ROWS);              \
                                                                              \
				for (BLASLONG j = 0; j < COLS; j++)           \
					Caux[i][j] += Ak * B[j + k * COLS];   \
			}                                                     \
		}                                                             \
                                                                              \
		/*                                                            \
		 * Unpack row-block of C_aux into outer C_i, multiply by      \
		 * alpha and add up.                                          \
		 */                                                           \
		for (BLASLONG j = 0; j < COLS; j++) {                         \
			for (BLASLONG i = 0; i < ROWS / VLEN_FLOATS; i++) {   \
				vector_float *C_ij =                          \
				    (vector_float *)(C + i * VLEN_FLOATS +    \
						     j * ldc);                \
				*C_ij += alpha * Caux[i][j];                  \
			}                                                     \
		}                                                             \
	}


VECTOR_BLOCK(8, 4)
VECTOR_BLOCK(8, 2)
VECTOR_BLOCK(8, 1)
VECTOR_BLOCK(4, 4)
VECTOR_BLOCK(4, 2)
VECTOR_BLOCK(4, 1)

#ifdef DOUBLE
VECTOR_BLOCK(2, 4)
VECTOR_BLOCK(2, 2)
#endif

/**
 * Handle calculation for row blocks in C_i of any size by dispatching into
 * macro-defined (inline) functions or by deferring to a simple generic
 * implementation. Note that the compiler can remove this awkward-looking
 * dispatching code while inlineing.
 *
 * @param[in]	m	Number of rows in block C_i.
 * @param[in]	n	Number of columns in block C_i.
 * @param[in]	first_row Index of first row of the block C_i (relative to C).
 * @param[in]	A	Pointer to input matrix A (note: all of it).
 * @param[in]	k	Number of columns in A and rows in B.
 * @param[in]	B	Pointer to current column block (panel) of input matrix B.
 * @param[inout] C	Pointer to current column block (panel) of output matrix C.
 * @param[in]	ldc	Offset between elements in adjacent columns in C.
 * @param[in]	alpha	Scalar factor.
 */
static inline void GEBP_block(BLASLONG m, BLASLONG n,
		       BLASLONG first_row,
		       const FLOAT * restrict A, BLASLONG k,
		       const FLOAT * restrict B,
		       FLOAT *restrict C, BLASLONG ldc,
		       FLOAT alpha)
{
	A += first_row * k;
	C += first_row;

#define BLOCK(bm, bn)                                           \
	if (m == bm && n == bn) {                               \
		GEBP_block_##bm##_##bn(A, k, B, C, ldc, alpha); \
		return;                                         \
	}

	BLOCK(8, 4); BLOCK(8, 2); BLOCK(8, 1);
	BLOCK(4, 4); BLOCK(4, 2); BLOCK(4, 1);

	#ifdef DOUBLE
	BLOCK(2, 4);
	BLOCK(2, 2);
	#endif

#undef BLOCK

	/* simple implementation for smaller block sizes: */
	FLOAT Caux[m][n] __attribute__ ((aligned (16)));

	/*
	 * Peel off first iteration (i.e., column of A) for initializing Caux
	 */
	for (BLASLONG i = 0; i < m; i++)
		for (BLASLONG j = 0; j < n; j++)
			Caux[i][j] = A[i] * B[j];

	for (BLASLONG kk = 1; kk < k; kk++)
		for (BLASLONG i = 0; i < m; i++)
			for (BLASLONG j = 0; j < n; j++)
				Caux[i][j] += A[i + kk * m] * B[j + kk * n];

	for (BLASLONG i = 0; i < m; i++)
		for (BLASLONG j = 0; j < n; j++)
			C[i + j * ldc] += alpha * Caux[i][j];
}

/**
 * Handle a column block (panel) of C and B while calculating C += alpha(A * B).
 *
 * @param[in]	num_cols	Number of columns in the block (in C and B).
 * @param[in]	first_col	First column of the current block (in C and B).
 * @param[in]	A	Pointer to input matrix A.
 * @param[in]	bk	Number of columns in A and rows in B.
 * @param[in]	B	Pointer to input matrix B (note: all of it).
 * @param[in]	bm	Number of rows in C and A.
 * @param[inout] C	Pointer to output matrix C (note: all of it).
 * @param[in]	ldc	Offset between elements in adjacent columns in C.
 * @param[in]	alpha	Scalar factor.
 */
static inline void GEBP_column_block(BLASLONG num_cols, BLASLONG first_col,
			const FLOAT *restrict A, BLASLONG bk,
			const FLOAT *restrict B, BLASLONG bm,
			FLOAT *restrict C, BLASLONG ldc,
			FLOAT alpha) {
	FLOAT *restrict C_i = C + first_col * ldc;
	/*
	 * B is in column-order with n_r packed row elements, which does
	 * not matter -- we always move in full such blocks of
	 * column*pack
	 */
	const FLOAT *restrict B_i = B + first_col * bk;

	/*
	 * Calculate C_aux := A * B_j
	 * then unpack C_i += alpha * C_aux.
	 *
	 * For that purpose, further partition C_aux and A into blocks
	 * of m_r (unroll_m) rows, or powers-of-2 if smaller.
	 */
	BLASLONG row = 0;
	for (BLASLONG block_size = unroll_m; block_size > 0; block_size /= 2)
		for (; bm - row >= block_size; row += block_size)
			GEBP_block(block_size, num_cols, row, A, bk, B_i, C_i,
				   ldc, alpha);
}

/**
 * Inner kernel for matrix-matrix multiplication. C += alpha (A * B)
 * where C is an m-by-n matrix, A is m-by-k and B is k-by-n. Note that A, B, and
 * C are pointers to submatrices of the actual matrices.
 *
 * @param[in]	bm	Number of rows in C and A.
 * @param[in]	bn	Number of columns in C and B.
 * @param[in]	bk	Number of columns in A and rows in B.
 * @param[in]	alpha	Scalar factor.
 * @param[in]	ba	Pointer to input matrix A.
 * @param[in]	bb	Pointer to input matrix B.
 * @param[inout] C	Pointer to output matrix C.
 * @param[in]	ldc	Offset between elements in adjacent columns in C.
 * @returns 0 on success.
 */
int CNAME(BLASLONG bm, BLASLONG bn, BLASLONG bk, FLOAT alpha,
	  FLOAT *restrict ba, FLOAT *restrict bb,
	  FLOAT *restrict C, BLASLONG ldc)
{
	if ( (bm == 0) || (bn == 0) || (bk == 0) || (alpha == ZERO))
		return 0;

	/*
	 * interface code allocates buffers for ba and bb at page
	 * granularity (i.e., using mmap(MAP_ANONYMOUS), so enable the compiler
	 * to make use of the fact in vector load operations.
	 */
	ba = __builtin_assume_aligned(ba, 16);
	bb = __builtin_assume_aligned(bb, 16);

	/*
	 * Partition B and C into blocks of n_r (unroll_n) columns, called B_i
	 * and C_i. For each partition, calculate C_i += alpha * (A * B_j).
	 *
	 * For remaining columns that do not fill up a block of n_r, iteratively
	 * use smaller block sizes of powers of 2.
	 */
	BLASLONG col = 0;
	for (BLASLONG block_size = unroll_n; block_size > 0; block_size /= 2)
		for (; bn - col >= block_size; col += block_size)
			GEBP_column_block(block_size, col, ba, bk, bb, bm, C, ldc, alpha);

   return 0;
}
