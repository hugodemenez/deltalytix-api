from fastapi import APIRouter, HTTPException
import logging
from typing import List
from app.models.trade import (
    ManualOrderSubmission,
    ManualOrderResponse,
    TempOrderResponse,
    ProcessBatchRequest,
    BatchStatusResponse,
    TempOrderStorage,
)
from app.services.manual_order_service import process_manual_orders
from app.services.trade_service import store_trades
from app.services.temp_storage_service import temp_storage
from app.core.security import decode_session_token

logger = logging.getLogger(__name__)
router = APIRouter()


@router.post("/store", response_model=TempOrderResponse)
async def store_manual_orders(submission: ManualOrderSubmission):
    """Store manual orders for later processing"""
    try:
        # Verify token and get user_id
        payload = decode_session_token(submission.token)
        user_id = payload.get("user_id")
        if not user_id:
            raise HTTPException(
                status_code=401, detail="Invalid token: missing user_id"
            )

        # Store orders
        batch_id = temp_storage.store_orders(user_id, submission.orders)

        return TempOrderResponse(
            success=True,
            message="Orders stored successfully",
            batch_id=batch_id,
            orders_stored=len(submission.orders),
        )

    except Exception as e:
        logger.error(f"Error storing manual orders: {e}")
        logger.exception(e)
        raise HTTPException(status_code=500, detail=str(e))


@router.post("/process", response_model=ManualOrderResponse)
async def process_batch(request: ProcessBatchRequest):
    """Process a specific batch of stored orders"""
    try:
        # Verify token and get user_id
        payload = decode_session_token(request.token)
        user_id = payload.get("user_id")
        if not user_id:
            raise HTTPException(
                status_code=401, detail="Invalid token: missing user_id"
            )

        # Get batch
        batch = temp_storage.get_batch(user_id, request.batch_id)
        if not batch:
            raise HTTPException(
                status_code=404, detail=f"Batch {request.batch_id} not found"
            )

        # Update status to processing
        temp_storage.update_batch_status(user_id, request.batch_id, "processing")

        try:
            # Process orders
            trades, open_positions = process_manual_orders(batch.orders, user_id)

            # Store trades
            if trades:
                store_trades(trades)

            # Update status to completed and delete batch
            temp_storage.update_batch_status(user_id, request.batch_id, "completed")
            temp_storage.delete_batch(user_id, request.batch_id)

            return ManualOrderResponse(
                success=True,
                message=f"Successfully processed batch {request.batch_id}",
                trades_created=len(trades),
                orders_processed=len(batch.orders),
            )

        except Exception as e:
            # Update status to error
            temp_storage.update_batch_status(user_id, request.batch_id, "error")
            raise

    except Exception as e:
        logger.error(f"Error processing batch: {e}")
        logger.exception(e)
        raise HTTPException(status_code=500, detail=str(e))


@router.get("/batches/{batch_id}", response_model=BatchStatusResponse)
async def get_batch_status(batch_id: str, token: str):
    """Get status of a specific batch"""
    try:
        # Verify token and get user_id
        payload = decode_session_token(token)
        user_id = payload.get("user_id")
        if not user_id:
            raise HTTPException(
                status_code=401, detail="Invalid token: missing user_id"
            )

        # Get batch
        batch = temp_storage.get_batch(user_id, batch_id)
        if not batch:
            raise HTTPException(status_code=404, detail=f"Batch {batch_id} not found")

        return BatchStatusResponse(
            success=True,
            message="Batch status retrieved successfully",
            batch_id=batch.batch_id,
            status=batch.status,
            orders_count=len(batch.orders),
            created_at=batch.created_at,
            last_updated=batch.last_updated,
        )

    except Exception as e:
        logger.error(f"Error getting batch status: {e}")
        logger.exception(e)
        raise HTTPException(status_code=500, detail=str(e))


@router.get("/batches", response_model=List[BatchStatusResponse])
async def get_user_batches(token: str):
    """Get all batches for a user"""
    try:
        # Verify token and get user_id
        payload = decode_session_token(token)
        user_id = payload.get("user_id")
        if not user_id:
            raise HTTPException(
                status_code=401, detail="Invalid token: missing user_id"
            )

        # Get all batches
        batches = temp_storage.get_user_batches(user_id)

        return [
            BatchStatusResponse(
                success=True,
                message="Batch retrieved successfully",
                batch_id=batch.batch_id,
                status=batch.status,
                orders_count=len(batch.orders),
                created_at=batch.created_at,
                last_updated=batch.last_updated,
            )
            for batch in batches
        ]

    except Exception as e:
        logger.error(f"Error getting user batches: {e}")
        logger.exception(e)
        raise HTTPException(status_code=500, detail=str(e))
