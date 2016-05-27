#ifndef NAST_HPX_GRID_UNPACK_BUFFER_HPP
#define NAST_HPX_GRID_UNPACK_BUFFER_HPP

#include <hpx/runtime/serialization/serialize_buffer.hpp>

#include "partition_data.hpp"

namespace nast_hpx { namespace grid {
    template <direction dir>
    struct unpack_buffer;

    template <>
    struct unpack_buffer<RIGHT>
    {
        template <typename BufferType>
        static void call(partition_data<Real>& p, BufferType buffer)
        {
            typename BufferType::value_type* src = buffer.data();
            
            HPX_ASSERT(buffer.size() == p.act_size_y_);
            

            for(std::size_t y = 1; y != p.size_y_ - 1; ++y)
            {
                p(p.size_x_ - 1, y) = *src;
                ++src;
            }           
        }
    };
    
}
}

#endif
