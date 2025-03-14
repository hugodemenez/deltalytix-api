from pydantic import BaseModel
from typing import Optional, List, Dict
from datetime import datetime


class Trade(BaseModel):
    id: str
    userId: str
    accountNumber: str
    instrument: str
    quantity: int
    entryPrice: str
    closePrice: str
    entryDate: str
    closeDate: str
    side: Optional[str]
    commission: float = 0
    timeInPosition: float = 0
    pnl: float
    entryId: str
    closeId: str
    comment: Optional[str]
    createdAt: datetime


class QueuedOrder(BaseModel):
    quantity: int
    price: float
    commission: float
    timestamp: str
    order_id: str
    side: str
    remaining: int


class OpenPosition(BaseModel):
    accountNumber: str
    instrument: str
    side: str
    quantity: int
    entryPrice: float
    entryDate: str
    commission: float
    orderId: str


class OrderRequest(BaseModel):
    username: str
    password: str
    server_type: str = "SpeedUp"  # Default value for server type
    server_name: str  # Location (e.g., "Chicago Area")
    start_date: str
    account_ids: Optional[List[str]] = None
    userId: str

    class Config:
        # This allows extra fields to be present in the request
        extra = "allow"

    @property
    def get_server_name(self) -> str:
        """Convert location to proper server name if needed"""
        # Map locations to server types if necessary
        location_map = {
            "Chicago Area": "SpeedUp",
            "Europe": "SpeedUp",
            # Add more mappings as needed
        }
        return location_map.get(self.server_name, self.server_type)


class OrderData(BaseModel):
    order_id: str
    account_id: str
    symbol: str
    exchange: str
    side: str
    order_type: str
    status: str
    quantity: int
    filled_quantity: int
    price: float
    commission: float
    timestamp: int


class OrderListResponse(BaseModel):
    success: bool
    message: str
    orders: List[OrderData] = []
    accounts_processed: int
    total_accounts_available: int


class Credentials(BaseModel):
    username: str
    password: str
    server_type: str
    location: str
    start_date: Optional[str] = None
    userId: str


class AccountData(BaseModel):
    account_id: str
    fcm_id: str
    ib_id: str


class AccountListResponse(BaseModel):
    success: bool
    message: str
    accounts: List[AccountData] = []
    websocket_url: str
    token: str


class ServerResponse(BaseModel):
    success: bool
    message: str
    servers: dict[str, List[str]]


class Instrument(BaseModel):
    Symbol: str
    Type: str


class ManualOrder(BaseModel):
    AccountId: str
    OrderId: str
    OrderState: str
    OrderAction: str
    OrderType: str
    LimitPrice: float
    StopPrice: Optional[float]
    Quantity: int
    AverageFilledPrice: float
    IsOpeningOrder: bool
    Time: datetime
    Instrument: Instrument


class ManualOrderSubmission(BaseModel):
    orders: List[ManualOrder]
    token: str


class ManualOrderResponse(BaseModel):
    success: bool
    message: str
    trades_created: int
    orders_processed: int


class TempOrderStorage(BaseModel):
    """Model for temporary order storage"""

    user_id: str
    orders: List[ManualOrder]
    created_at: datetime
    last_updated: datetime
    status: str = "pending"  # pending, processing, completed, error
    batch_id: str  # To identify different batches from same user


class TempOrderResponse(BaseModel):
    """Response for temporary order storage"""

    success: bool
    message: str
    batch_id: str
    orders_stored: int


class ProcessBatchRequest(BaseModel):
    """Request to process a specific batch"""

    token: str
    batch_id: str


class BatchStatusResponse(BaseModel):
    """Response for batch status check"""

    success: bool
    message: str
    batch_id: str
    status: str
    orders_count: int
    created_at: datetime
    last_updated: datetime


class ProcessOrderResponse(BaseModel):
    success: bool
    message: str
    process_id: str


class ProcessStatusResponse(BaseModel):
    process_id: str
    status: str
    started_at: Optional[datetime]
    completed_at: Optional[datetime]
    error: Optional[str]
    result: Optional[Dict]
