/*
 Copyright 2016-2020 Intel Corporation
 
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at
 
     http://www.apache.org/licenses/LICENSE-2.0
 
 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/
#include "coll/algorithms/reduce_scatter/sycl/reduce_scatter_large_sycl.hpp"

#define REDUCE_SCATTER_LARGE_API_DECL(TYPE) \
    void init_reduce_scatter_large_##TYPE(ccl::datatype dtype, \
                                          sycl::queue &queue, \
                                          ccl_comm *comm, \
                                          ccl_stream *stream, \
                                          uint32_t rank_in, \
                                          uint32_t world_in); \
    ccl::event run_reduce_scatter_large_##TYPE(ccl::datatype dtype, \
                                               sycl::queue &queue, \
                                               const void *send_buf, \
                                               void *recv_buf, \
                                               size_t recv_count, \
                                               bool &done);

REDUCE_SCATTER_LARGE_API_DECL(fp16);
REDUCE_SCATTER_LARGE_API_DECL(bf16);
REDUCE_SCATTER_LARGE_API_DECL(fp32);
REDUCE_SCATTER_LARGE_API_DECL(int32);

#define SWITCH_INIT_TYPE(TYPE, ccl_type) \
    case ccl_type: init_reduce_scatter_large_##TYPE(dtype, queue, comm, stream, rank_in, world_in); break;

void init_reduce_scatter_large(ccl::datatype dtype,
                               sycl::queue &queue,
                               ccl_comm *comm,
                               ccl_stream *stream,
                               uint32_t rank_in,
                               uint32_t world_in) {
    switch (dtype) {
        SWITCH_INIT_TYPE(fp16, ccl::datatype::float16)
        SWITCH_INIT_TYPE(bf16, ccl::datatype::bfloat16)
        SWITCH_INIT_TYPE(fp32, ccl::datatype::float32)
        SWITCH_INIT_TYPE(int32, ccl::datatype::int32)
        default: assert(0);
    }
}

#define SWITCH_RUN_TYPE(TYPE, ccl_type) \
    case ccl_type: e = run_reduce_scatter_large_##TYPE(dtype, queue, send_buf, recv_buf, recv_count, done); break;

ccl::event run_reduce_scatter_large(ccl::datatype dtype,
                                    sycl::queue &queue,
                                    const void *send_buf,
                                    void *recv_buf,
                                    size_t recv_count,
                                    bool &done) {
    ccl::event e;
    switch (dtype) {
        SWITCH_RUN_TYPE(fp16, ccl::datatype::float16)
        SWITCH_RUN_TYPE(bf16, ccl::datatype::bfloat16)
        SWITCH_RUN_TYPE(fp32, ccl::datatype::float32)
        SWITCH_RUN_TYPE(int32, ccl::datatype::int32)
        default: assert(0);
    }
    return e;
}

#include "coll/algorithms/reduce_scatter/sycl/reduce_scatter_large_sycl_impl.hpp"

ccl::event reduce_scatter_large(const void *send_buf,
                                void *recv_buf,
                                size_t recv_count,
                                ccl::datatype dtype,
                                ccl::reduction reduction,
                                ccl_comm *comm,
                                ccl_stream *global_stream,
                                const ccl::vector_class<ccl::event> &deps) {
    LOG_DEBUG("invoking reduce_scatter_large");

    std::shared_ptr<ccl_comm> pair_comm = comm->get_pair_comm();
    std::shared_ptr<ccl_comm> even_comm = comm->get_even_comm();

    const size_t dsize = ccl::global_data::get().dtypes->get(dtype).size();
    const bool is_odd = recv_count % 2 != 0 && dsize < sizeof(int);
    const bool is_aligned = (recv_count * dsize) % ccl::global_data::env().kernel_mem_align == 0;
    const bool is_use_tmp = ccl::global_data::env().sycl_reduce_scatter_tmp_buf || is_odd ||
                            (!is_aligned && ccl::global_data::env().sycl_auto_use_tmp_buf);

    if (!is_use_tmp) {
        std::vector<void *> ptrs{ (void *)send_buf, recv_buf }; // index 0 and 1
        auto [sched, exchange_entry] = do_ipc_exchange(comm, global_stream, ptrs);

        xelink_ptrs_rd = get_ipc_ptrs<void, MAX_GPUS>(even_comm, 0, (void *)send_buf, sched);

        if (pair_comm->size() > 1) {
            assert(pair_comm->size() == MAX_TILES);
            int peer_pair_rank = pair_comm->rank() ? 0 : 1;
            mdfi_ptr_rd = get_ipc_ptrs<void, MAX_TILES>(pair_comm, 0, (void *)send_buf, sched)[peer_pair_rank];
        }
        delete exchange_entry;
        delete sched;

        coll_init(comm, global_stream);
    }
    else {
        coll_init(comm, global_stream);
        // 0 index is used for tmp work buffer and
        // 1 index is used to copy input data
        xelink_ptrs_rd = get_remote_even_tmp_buf(1);
        if (pair_comm->size() > 1) {
            assert(pair_comm->size() == MAX_TILES);
            int peer_pair_rank = pair_comm->rank() ? 0 : 1;
            mdfi_ptr_rd = get_remote_pair_tmp_buf(1)[peer_pair_rank];
        }
    }

    auto lambda = [&]<typename T, int NE, int NP>() {
        if (is_odd) {
            return reduce_scatter_large_impl<T, NE, NP, true>(
                send_buf, recv_buf, recv_count, dtype, reduction, comm, global_stream, deps);
        }
        else {
            return reduce_scatter_large_impl<T, NE, NP, false>(
                send_buf, recv_buf, recv_count, dtype, reduction, comm, global_stream, deps);
        }
    };

    return invoke_collective(lambda, comm, dtype);
}
