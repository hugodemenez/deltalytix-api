# RApiPlus Python Bindings

This package provides Python bindings for the RApiPlus trading API using pybind11.

## Prerequisites

- CMake 3.4 or higher
- Python 3.6 or higher with development headers
- C++ compiler with C++17 support
- RApiPlus library and dependencies
- pybind11 (included as a submodule)

### Installing Dependencies

#### macOS
```bash
# Install Homebrew if not already installed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install CMake and Python
brew install cmake
brew install python@3.11  # or your preferred Python version

# Install Xcode Command Line Tools (for C++ compiler)
xcode-select --install
```

#### Ubuntu/Debian
```bash
# Update package list
sudo apt update

# Install build essentials and CMake
sudo apt install build-essential cmake

# Install Python development headers
sudo apt install python3-dev

# Optional: Install specific Python version
sudo apt install python3.11-dev  # adjust version as needed
```

#### Windows
1. Install Visual Studio 2019 or later with C++ development tools
2. Install CMake:
   - Download from https://cmake.org/download/
   - Add to PATH during installation
3. Install Python:
   - Download from https://www.python.org/downloads/
   - Make sure to check "Add Python to PATH"
   - Select "Install Python development files"

## Building the Module

1. Clone the repository with submodules:
   ```bash
   git clone --recursive <repository-url>
   cd <repository-name>
   ```

   If you already cloned without submodules:
   ```bash
   git submodule update --init --recursive
   ```

2. Create and enter the build directory:
   ```bash
   mkdir build
   cd build
   ```

3. Configure with CMake:
   ```bash
   # Basic configuration
   cmake ..

   # Or with specific Python version
   cmake -DPYTHON_EXECUTABLE=$(which python3.11) ..

   # Or with custom install location
   cmake -DCMAKE_INSTALL_PREFIX=/custom/path ..
   ```

4. Build the module:
   ```bash
   make -j$(nproc)  # Linux/macOS
   # or
   cmake --build . --config Release  # Windows
   ```

5. Install (optional):
   ```bash
   make install  # might need sudo
   # or
   cmake --build . --target install
   ```

The Python module will be built in the `python` directory.

### Common Build Issues

1. **CMake not found**:
   - Check if CMake is installed: `cmake --version`
   - If not found, install using instructions above
   - Make sure CMake is in your PATH

2. **Python development headers not found**:
   - Install python-dev package for your Python version
   - Or use `python3-config --includes` to verify headers location

3. **C++ compiler issues**:
   - Make sure you have a C++17 compatible compiler
   - Install build-essential (Linux) or Xcode Command Line Tools (macOS)

4. **pybind11 issues**:
   - Make sure you cloned with `--recursive`
   - Or run `git submodule update --init --recursive`

### Verifying Installation

```python
import rapi

# Check version
print(rapi.REngine.get_version())

# Create engine instance
engine = rapi.REngine("TestApp", "1.0.0")
```

## Usage

### Basic Example

```python
import rapi

# Create an REngine instance
engine = rapi.REngine("MyApp", "1.0.0")

try:
    # Login to the trading system
    success = engine.login(
        user="your_username",
        password="your_password",
        mdCnnctPt="",  # Empty for no market data
        tsCnnctPt="trading_system_connection_point"
    )
    
    if success:
        print("Successfully logged in")
        
        # Create an account info object
        account = rapi.AccountInfo()
        account.fcm_id = "FCM123"
        account.ib_id = "IB456"
        account.account_id = "ACC789"
        
        # Work with the API...
        
finally:
    # Always logout when done
    engine.logout()
```

### Full Order Fetcher Example

The package includes a complete `OrderFetcher` class that demonstrates all functionality:

```python
from rapi import OrderFetcher

# Create fetcher instance
fetcher = OrderFetcher(
    username="user",
    password="pass",
    server_type="demo",
    location="us_east",
    start_date="20240101",
    account_ids=["ACC1", "ACC2"]  # Optional: specific accounts to process
)

# Run the fetcher
fetcher.run()
```

## API Reference

### Classes

#### REngine
- `__init__(app_name: str, app_version: str)`: Initialize the REngine
- `set_callbacks(on_account_list, on_order_replay, on_order_history_dates, on_product_rms_list, on_alert)`: Set callback functions
- `login(user: str, password: str, md_cnnct_pt: str, ts_cnnct_pt: str) -> bool`: Login to the trading system
- `logout() -> bool`: Logout from the trading system
- `get_accounts() -> bool`: Request account list
- `replay_all_orders(account: AccountInfo, start_ssboe: int, end_ssboe: int) -> bool`: Replay current session orders
- `replay_historical_orders(account: AccountInfo, date: str) -> bool`: Replay orders for a specific date
- `list_order_history_dates(account: AccountInfo) -> bool`: Get available history dates
- `get_product_rms_info(account: AccountInfo) -> bool`: Get commission rates
- `subscribe_order(account: AccountInfo) -> bool`: Subscribe to order updates
- `unsubscribe_order(account: AccountInfo) -> bool`: Unsubscribe from order updates
- `get_error_string(error_code: int) -> str`: Get error message for error code
- `get_version() -> str`: Get RApi version

#### AccountInfo
- Properties:
  - `fcm_id: str`
  - `ib_id: str`
  - `account_id: str`
  - `account_name: str`
  - `creation_ssboe: int`
  - `creation_usecs: int`

#### OrderData
- Properties:
  - `order_id: str`
  - `account_id: str`
  - `symbol: str`
  - `exchange: str`
  - `side: str`
  - `order_type: str`
  - `status: str`
  - `quantity: int`
  - `filled_quantity: int`
  - `price: float`
  - `commission: float`
  - `timestamp: int`

#### CommissionRate
- Properties:
  - `rate: float`
  - `is_valid: bool`
- Methods:
  - `__init__(rate: float = 0.0, is_valid: bool = False)`

#### ProcessingStats
- Properties:
  - `total_days: int`
  - `days_processed: int`
  - `orders_processed: int`
- Methods:
  - `__init__(total: int = 0, processed: int = 0, orders: int = 0)`

### Constants

#### Alert Types
- `ALERT_CONNECTION_OPENED`
- `ALERT_CONNECTION_CLOSED`
- `ALERT_LOGIN_COMPLETE`
- `ALERT_LOGIN_FAILED`

#### Connection IDs
- `MARKET_DATA_CONNECTION_ID`
- `TRADING_SYSTEM_CONNECTION_ID`
- `PNL_CONNECTION_ID`

#### Order Types
- `ORDER_TYPE_LIMIT`
- `ORDER_TYPE_MARKET`
- `ORDER_TYPE_STOP_MARKET`
- `ORDER_TYPE_STOP_LIMIT`

#### Buy/Sell Types
- `BUY_SELL_TYPE_BUY`
- `BUY_SELL_TYPE_SELL`
- `BUY_SELL_TYPE_SELL_SHORT`

#### Order Duration
- `ORDER_DURATION_DAY`
- `ORDER_DURATION_GTC`

#### Line Status
- `LINE_STATUS_OPEN`
- `LINE_STATUS_COMPLETE`
- `LINE_STATUS_CANCEL_PENDING`
- `LINE_STATUS_MODIFY_PENDING`

#### Return Codes
- `OK`
- `BAD`
- `API_OK`

## OrderFetcher Class

### Methods
- `__init__(username: str, password: str, server_type: str, location: str, start_date: str, account_ids: Optional[List[str]] = None)`
- `run() -> bool`: Main execution method
- `process_account(account: AccountInfo) -> bool`: Process a single account
- `save_orders() -> str`: Save orders to JSON file
- `wait_for_condition(condition_var: str, timeout: int = 30) -> bool`: Wait for async operations

### Callbacks
- `on_account_list(accounts: List[AccountInfo])`
- `on_order_replay(orders: List[OrderData])`
- `on_order_history_dates(dates: List[str])`
- `on_product_rms_info(commission_rates: Dict[str, float])`
- `on_alert(alert_type: int, message: str)`

### Output Format
```json
{
    "status": "complete",
    "timestamp": "1234567890",
    "total_accounts_available": 10,
    "accounts_processed": 5,
    "total_orders": 1000,
    "orders": {
        "account1": [
            {
                "order_id": "ORD001",
                "account_id": "ACC1",
                "symbol": "AAPL",
                "exchange": "NASDAQ",
                "side": "BUY",
                "order_type": "LIMIT",
                "status": "complete",
                "quantity": 100,
                "filled_quantity": 100,
                "price": 150.0,
                "commission": 1.5,
                "timestamp": 1234567890
            }
        ]
    }
}
```

## Error Handling

The library includes comprehensive error handling:
- Timeout handling for async operations
- Retry logic for subscriptions
- Error code to string conversion
- Exception handling with proper cleanup

## Thread Safety

The library is designed to be thread-safe:
- Proper resource cleanup in destructors
- Safe callback handling
- Mutex protection for shared resources

## License

This project is licensed under the same terms as the RApiPlus library. 