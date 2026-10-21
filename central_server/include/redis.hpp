#pragma once

#include <boost/redis/src.hpp>
#include <memory>

namespace Redis {
using boost::Redis::Connection;

class MemoryCache {
   private:
    std::shared_ptr<Connection> m_redis;
};
}  // namespace Redis
