from celery import Celery
from celery.schedules import crontab
import os

# Initialize Celery app
celery_app = Celery(
    "deltalytix",
    broker=os.getenv("REDIS_URL", "redis://localhost:6379/0"),
    backend=os.getenv("REDIS_URL", "redis://localhost:6379/0"),
    include=["app.tasks"],
)

# Optional configuration
celery_app.conf.update(
    task_serializer="json",
    accept_content=["json"],
    result_serializer="json",
    timezone="UTC",
    enable_utc=True,
    task_track_started=True,
    task_time_limit=300,  # 5 minutes
)

# Optional: Configure periodic tasks if needed
celery_app.conf.beat_schedule = {
    # Add periodic tasks here if needed
}
