import os
import json
import logging
from typing import Optional, Any, Set, Dict, List
import aioredis
from aioredis import Redis
from fastapi import WebSocket

logger = logging.getLogger(__name__)


class RedisManager:
    _instance = None
    _redis: Optional[Redis] = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super(RedisManager, cls).__new__(cls)
        return cls._instance

    async def initialize(self):
        if self._redis is None:
            try:
                redis_host = os.getenv("REDIS_HOST", "redis")
                redis_port = int(os.getenv("REDIS_PORT", 6379))

                self._redis = await aioredis.from_url(
                    f"redis://{redis_host}:{redis_port}",
                    encoding="utf-8",
                    decode_responses=True,
                )
                logger.info(f"Connected to Redis at {redis_host}:{redis_port}")
            except Exception as e:
                logger.error(f"Failed to connect to Redis: {e}")
                raise

    @property
    def redis(self) -> Redis:
        if self._redis is None:
            raise RuntimeError("Redis connection not initialized")
        return self._redis

    async def close(self):
        if self._redis:
            await self._redis.close()
            self._redis = None

    # Session Management
    async def add_session_connection(self, session_id: str, client_id: str) -> None:
        """Add a client connection to a session"""
        await self.redis.sadd(f"session:{session_id}:clients", client_id)

    async def remove_session_connection(self, session_id: str, client_id: str) -> None:
        """Remove a client connection from a session"""
        await self.redis.srem(f"session:{session_id}:clients", client_id)
        # Clean up session if no clients left
        if not await self.redis.scard(f"session:{session_id}:clients"):
            await self.cleanup_session(session_id)

    async def get_session_connections(self, session_id: str) -> Set[str]:
        """Get all client connections for a session"""
        return await self.redis.smembers(f"session:{session_id}:clients")

    # State Management
    async def set_session_state(self, session_id: str, state: dict) -> None:
        """Store session state"""
        await self.redis.set(f"session:{session_id}:state", json.dumps(state))

    async def get_session_state(self, session_id: str) -> Optional[dict]:
        """Retrieve session state"""
        state = await self.redis.get(f"session:{session_id}:state")
        return json.loads(state) if state else None

    # Message Cache
    async def cache_message(self, session_id: str, message: dict) -> None:
        """Cache a message for a session"""
        await self.redis.rpush(f"session:{session_id}:messages", json.dumps(message))
        # Trim to last 1000 messages
        await self.redis.ltrim(f"session:{session_id}:messages", -1000, -1)

    async def get_cached_messages(self, session_id: str) -> List[dict]:
        """Get all cached messages for a session"""
        messages = await self.redis.lrange(f"session:{session_id}:messages", 0, -1)
        return [json.loads(msg) for msg in messages]

    # Order Cache
    async def cache_order(
        self, session_id: str, order_id: str, order_data: dict
    ) -> None:
        """Cache an order for a session"""
        await self.redis.hset(
            f"session:{session_id}:orders", order_id, json.dumps(order_data)
        )

    async def get_cached_orders(self, session_id: str) -> Dict[str, dict]:
        """Get all cached orders for a session"""
        orders = await self.redis.hgetall(f"session:{session_id}:orders")
        return {k: json.loads(v) for k, v in orders.items()}

    # Cleanup
    async def cleanup_session(self, session_id: str) -> None:
        """Clean up all session data"""
        keys = await self.redis.keys(f"session:{session_id}:*")
        if keys:
            await self.redis.delete(*keys)


# Global Redis manager instance
redis_manager = RedisManager()
