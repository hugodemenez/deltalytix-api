from fastapi import WebSocket
from typing import Set, Dict, Optional
from pydantic import BaseModel


class WebSocketMessage(BaseModel):
    type: str
    message: Optional[str] = None
    level: Optional[str] = None
    data: Optional[dict] = None
    timestamp: Optional[int] = None


class WebSocketState(BaseModel):
    session_id: str
    status: str
    started_at: Optional[str] = None
    credentials: Optional[dict] = None
    accounts: Optional[list] = None
