/*
 * Hemi.hpp
 *
 */

#ifndef PROTOCOLS_HEMI_HPP_
#define PROTOCOLS_HEMI_HPP_

#include "Hemi.h"
#include "ShareMatrix.h"
#include "HemiOptions.h"

#include "HemiMatrixPrep.hpp"
#include "HemiPrep.hpp"

template<class T>
Hemi<T>::~Hemi()
{
    for (auto& x : matrix_preps)
        delete x.second;
}

template<class T>
typename T::MatrixPrep& Hemi<T>::get_matrix_prep(const array<int, 3>& dims,
        SubProcessor<T>& processor)
{
    if (matrix_preps.find(dims) == matrix_preps.end())
        matrix_preps.insert(pair<array<int, 3>, typename T::MatrixPrep*>(dims,
            new typename T::MatrixPrep(dims[0], dims[1], dims[2],
                    dynamic_cast<typename T::LivePrep&>(processor.DataF),
                    matrix_usage)));
    return *matrix_preps.at(dims);
}

template<class T>
void Hemi<T>::matmulsm(SubProcessor<T>& processor, MemoryPart<T>& source,
        const Instruction& instruction)
{
    if (HemiOptions::singleton.plain_matmul
            or not OnlineOptions::singleton.live_prep)
    {
        processor.matmulsm(source, instruction);
        return;
    }

    // Perform the matrix multiplications in sequence.
    // They are not merged into one communication round since that would require multiple matrix_preps to
    // merge rounds.
    // An improvement might be to merge the communication of multiple matrices with the same dimension into one round,
    // which is not implemented yet.
    auto Proc = processor.Proc;
    assert(Proc);
    auto& S = processor.get_S();
    auto& start = instruction.get_start();

    for (auto matmulArgs = start.begin(); matmulArgs < start.end(); matmulArgs += 12) {
        auto C = S.begin() + matmulArgs[0];
        size_t firstFactorBase  = Proc->get_Ci().at(matmulArgs[1]).get();
        size_t secondFactorBase = Proc->get_Ci().at(matmulArgs[2]).get();
        auto resultNumberOfRows = matmulArgs[3];
        auto usedNumberOfFirstFactorColumns = matmulArgs[4];
        auto resultNumberOfColumns = matmulArgs[5];
        auto firstFactorTotalNumberOfColumns = matmulArgs[10];
        auto secondFactorTotalNumberOfColumns = matmulArgs[11];

        assert(C + resultNumberOfRows * resultNumberOfColumns <= S.end());

        ShareMatrix<T> A(resultNumberOfRows, usedNumberOfFirstFactorColumns), B(usedNumberOfFirstFactorColumns, resultNumberOfColumns);
        if (not T::real_shares(processor.P))
        {
            matrix_multiply(A, B, processor);
            return;
        }

        for (int i = 0; i < resultNumberOfRows; i++) {
            auto actualFirstFactorRow = Proc->get_Ci().at(matmulArgs[6] + i).get();

            for (int k = 0; k < usedNumberOfFirstFactorColumns; k++)
            {
                auto actualFirstFactorColumn = Proc->get_Ci().at(matmulArgs[7] + k).get();
                A.entries.v.push_back(source.at(firstFactorBase + actualFirstFactorRow * firstFactorTotalNumberOfColumns + actualFirstFactorColumn));
            }
        }


        for (int k = 0; k < usedNumberOfFirstFactorColumns; k++) {
            auto actualSecondFactorRow = Proc->get_Ci().at(matmulArgs[8] + k).get();
            for (int j = 0; j < resultNumberOfColumns; j++)
            {
                auto actualSecondFactorColumn = Proc->get_Ci().at(matmulArgs[9] + j).get();
                B.entries.v.push_back(source.at(secondFactorBase + actualSecondFactorRow * secondFactorTotalNumberOfColumns + actualSecondFactorColumn));
            }
        }

        auto res = matrix_multiply(A, B, processor);

        for (int i = 0; i < resultNumberOfRows; i++)
            for (int j = 0; j < resultNumberOfColumns; j++)
                *(C + i * resultNumberOfColumns + j) = res[{i, j}];
    }
}

template<class T>
ShareMatrix<T> Hemi<T>::matrix_multiply(const ShareMatrix<T>& A,
        const ShareMatrix<T>& B, SubProcessor<T>& processor)
{
    Beaver<ShareMatrix<T>> beaver(this->P);
    array<int, 3> dims = {{A.n_rows, A.n_cols, B.n_cols}};
    ShareMatrix<T> C(A.n_rows, B.n_cols);

    int max_inner = OnlineOptions::singleton.batch_size;
    int max_cols = OnlineOptions::singleton.batch_size;
    for (int i = 0; i < A.n_cols; i += max_inner)
    {
        for (int j = 0; j < B.n_cols; j += max_cols)
        {
            auto subdim = dims;
            subdim[1] = min(max_inner, A.n_cols - i);
            subdim[2] = min(max_cols, B.n_cols - j);
            auto& prep = get_matrix_prep(subdim, processor);
            beaver.init(prep, mc);
            beaver.init_mul();
            bool for_real = T::real_shares(processor.P);
            beaver.prepare_mul(A.from(0, i, subdim.data(), for_real),
                    B.from(i, j, subdim.data() + 1, for_real));
            if (for_real)
            {
                beaver.exchange();
                C.add_from_col(j, beaver.finalize_mul());
            }
        }
    }

    return C;
}

/**
 * Reduce convolution to matrix multiplication
 */
template<class T>
void Hemi<T>::conv2ds(SubProcessor<T>& processor,
        const Instruction& instruction)
{
    if (HemiOptions::singleton.plain_matmul
            or not OnlineOptions::singleton.live_prep)
    {
        processor.conv2ds(instruction);
        return;
    }

    auto& args = instruction.get_start();
    vector<Conv2dTuple> tuples;
    for (size_t i = 0; i < args.size(); i += 15)
        tuples.push_back(Conv2dTuple(args, i));
    for (auto& tuple : tuples)
        tuple.run_matrix(processor);
}

template<class T>
void Conv2dTuple::run_matrix(SubProcessor<T>& processor)
{
    auto& S = processor.get_S();
    array<int, 3> dim({{1, weights_h * weights_w * n_channels_in, batch_size * output_h * output_w}});
    ShareMatrix<T> A(dim[0], dim[1]), B(dim[1], dim[2]);

    if (not T::real_shares(processor.P))
    {
        processor.protocol.matrix_multiply(A, B, processor);
        return;
    }

    A.entries.init();
    B.entries.init();

    for (int i_batch = 0; i_batch < batch_size; i_batch ++)
    {
        size_t base = r1 + i_batch * inputs_w * inputs_h * n_channels_in;
        assert(base + inputs_w * inputs_h * n_channels_in <= S.size());
        T* input_base = &S[base];
        for (int out_y = 0; out_y < output_h; out_y++)
            for (int out_x = 0; out_x < output_w; out_x++)
            {
                int in_x_origin = (out_x * stride_w) - padding_w;
                int in_y_origin = (out_y * stride_h) - padding_h;

                for (int filter_y = 0; filter_y < weights_h; filter_y++)
                {
                    int in_y = in_y_origin + filter_y * filter_stride_h;
                    if ((0 <= in_y) and (in_y < inputs_h))
                        for (int filter_x = 0; filter_x < weights_w; filter_x++)
                        {
                            int in_x = in_x_origin + filter_x * filter_stride_w;
                            if ((0 <= in_x) and (in_x < inputs_w))
                            {
                                T* pixel_base = &input_base[(in_y * inputs_w
                                        + in_x) * n_channels_in];
                                T* weight_base = &S[r2
                                        + (filter_y * weights_w + filter_x)
                                                * n_channels_in];
                                for (int in_c = 0; in_c < n_channels_in; in_c++)
//                                    protocol.prepare_dotprod(pixel_base[in_c],
//                                            weight_base[in_c])
                                {
                                    int i_inner = n_channels_in * (filter_x * weights_h + filter_y) + in_c;
                                    B[{i_inner, output_h * (output_w * i_batch + out_x) + out_y}] = pixel_base[in_c];
                                    A[{0, i_inner}] = weight_base[in_c];
                                }
                            }
                        }
                }
//
//                protocol.next_dotprod();
            }
    }

    auto C = processor.protocol.matrix_multiply(A, B, processor);

    for (int i_batch = 0; i_batch < batch_size; i_batch ++)
    {
        size_t base = r0 + i_batch * output_h * output_w;
        assert(base + output_h * output_w <= S.size());
        T* output_base = &S[base];
        for (int out_y = 0; out_y < output_h; out_y++)
            for (int out_x = 0; out_x < output_w; out_x++)
            {
                output_base[out_y * output_w + out_x] = C[{0, output_h * (output_w * i_batch + out_x) + out_y}];
//                        protocol.finalize_dotprod(
//                                lengths[i_batch][out_y][out_x]);
            }
    }

}

#endif /* PROTOCOLS_HEMI_HPP_ */
