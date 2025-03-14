import psycopg2
import asyncpg
from psycopg2.extras import RealDictCursor
import logging
from app.core.config import settings

logger = logging.getLogger(__name__)


class DatabaseManager:
    def __init__(self):
        self._connection = None
        self._async_pool = None

    def get_connection(self):
        if self._connection is None or self._connection.closed:
            try:
                self._connection = psycopg2.connect(
                    dbname=settings.POSTGRES_DB,
                    user=settings.POSTGRES_USER,
                    password=settings.POSTGRES_PASSWORD,
                    host=settings.POSTGRES_HOST,
                    port=settings.POSTGRES_PORT,
                    # Connection pooling settings
                    keepalives=1,
                    keepalives_idle=30,
                    keepalives_interval=10,
                    keepalives_count=5,
                )
                self._connection.autocommit = False
                logger.info("Successfully connected to PostgreSQL via pooler")
            except Exception as e:
                logger.error(f"Failed to connect to PostgreSQL: {e}")
                raise
        return self._connection

    async def get_async_pool(self):
        """Get or create the async connection pool"""
        if self._async_pool is None:
            try:
                self._async_pool = await asyncpg.create_pool(
                    database=settings.POSTGRES_DB,
                    user=settings.POSTGRES_USER,
                    password=settings.POSTGRES_PASSWORD,
                    host=settings.POSTGRES_HOST,
                    port=settings.POSTGRES_PORT,
                    min_size=5,
                    max_size=20,
                )
                logger.info("Successfully created async PostgreSQL connection pool")
            except Exception as e:
                logger.error(f"Failed to create async PostgreSQL pool: {e}")
                raise
        return self._async_pool

    def get_cursor(self, cursor_factory=RealDictCursor):
        """Get a cursor with the specified factory"""
        return self.get_connection().cursor(cursor_factory=cursor_factory)

    async def close_async_pool(self):
        """Close the async connection pool"""
        if self._async_pool is not None:
            await self._async_pool.close()
            self._async_pool = None

    def close(self):
        """Close the database connection"""
        if self._connection is not None:
            self._connection.close()
            self._connection = None


# Create a global instance of DatabaseManager
db = DatabaseManager()
