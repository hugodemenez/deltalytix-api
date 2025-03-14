import logging
import json
import os
from typing import Dict, List, Optional
from datetime import datetime
import uuid
from app.models.trade import TempOrderStorage, ManualOrder
from app.core.config import settings

logger = logging.getLogger(__name__)


class TempStorageService:
    def __init__(self):
        self.storage_dir = os.path.join(os.getcwd(), "temp_db")
        os.makedirs(self.storage_dir, exist_ok=True)
        self._ensure_storage_exists()

    def _ensure_storage_exists(self):
        """Ensure storage directory exists"""
        if not os.path.exists(self.storage_dir):
            os.makedirs(self.storage_dir)

    def _get_user_file_path(self, user_id: str) -> str:
        """Get path to user's storage file"""
        return os.path.join(self.storage_dir, f"{user_id}_orders.json")

    def _load_user_data(self, user_id: str) -> Dict[str, TempOrderStorage]:
        """Load user's stored orders"""
        try:
            file_path = self._get_user_file_path(user_id)
            if not os.path.exists(file_path):
                return {}

            with open(file_path, "r") as f:
                data = json.load(f)
                return {
                    batch_id: TempOrderStorage(**batch_data)
                    for batch_id, batch_data in data.items()
                }
        except Exception as e:
            logger.error(f"Error loading user data: {e}")
            return {}

    def _save_user_data(self, user_id: str, data: Dict[str, TempOrderStorage]):
        """Save user's stored orders"""
        try:
            file_path = self._get_user_file_path(user_id)
            with open(file_path, "w") as f:
                json_data = {batch_id: batch.dict() for batch_id, batch in data.items()}
                json.dump(json_data, f, default=str)
        except Exception as e:
            logger.error(f"Error saving user data: {e}")
            raise

    def store_orders(self, user_id: str, orders: List[ManualOrder]) -> str:
        """Store orders for later processing"""
        try:
            # Load existing data
            user_data = self._load_user_data(user_id)

            # Create new batch
            batch_id = str(uuid.uuid4())
            new_batch = TempOrderStorage(
                user_id=user_id,
                orders=orders,
                created_at=datetime.now(),
                last_updated=datetime.now(),
                batch_id=batch_id,
            )

            # Add new batch
            user_data[batch_id] = new_batch

            # Save updated data
            self._save_user_data(user_id, user_data)

            return batch_id

        except Exception as e:
            logger.error(f"Error storing orders: {e}")
            raise

    def get_batch(self, user_id: str, batch_id: str) -> Optional[TempOrderStorage]:
        """Get a specific batch of orders"""
        try:
            user_data = self._load_user_data(user_id)
            return user_data.get(batch_id)
        except Exception as e:
            logger.error(f"Error getting batch: {e}")
            return None

    def update_batch_status(self, user_id: str, batch_id: str, status: str):
        """Update the status of a batch"""
        try:
            user_data = self._load_user_data(user_id)
            if batch_id in user_data:
                user_data[batch_id].status = status
                user_data[batch_id].last_updated = datetime.now()
                self._save_user_data(user_id, user_data)
        except Exception as e:
            logger.error(f"Error updating batch status: {e}")
            raise

    def get_user_batches(self, user_id: str) -> List[TempOrderStorage]:
        """Get all batches for a user"""
        try:
            user_data = self._load_user_data(user_id)
            return list(user_data.values())
        except Exception as e:
            logger.error(f"Error getting user batches: {e}")
            return []

    def delete_batch(self, user_id: str, batch_id: str):
        """Delete a batch after processing"""
        try:
            user_data = self._load_user_data(user_id)
            if batch_id in user_data:
                del user_data[batch_id]
                self._save_user_data(user_id, user_data)
        except Exception as e:
            logger.error(f"Error deleting batch: {e}")
            raise


# Create global instance
temp_storage = TempStorageService()
