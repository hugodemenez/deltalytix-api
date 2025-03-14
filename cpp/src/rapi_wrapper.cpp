#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include "RApiPlus.h"
#include <map>
#include <vector>
#include <string>
#include <memory>
#include <unistd.h>  // For access()
#include <sys/stat.h>  // For mkdir(), chmod(), stat
#include <errno.h>  // For errno and strerror
#include <string.h>  // For strerror
#include <stdio.h>  // For FILE operations
#include <iostream>  // For std::cerr
#include <nlohmann/json.hpp>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace py = pybind11;
using namespace RApi;  // Add this line to use RApi namespace

// Interface for login completion callback
class LoginCompletionHandler {
public:
    virtual void on_login_complete(int alert_type, int connection_id) = 0;
    virtual ~LoginCompletionHandler() = default;
};

// Interface for account list callback
class AccountListHandler {
public:
    virtual void on_account_list_received() = 0;
    virtual ~AccountListHandler() = default;
};

// Forward declare PyREngine
class PyREngine;

// Helper function to convert tsNCharcb to Python string
std::string tsNCharcb_to_string(const tsNCharcb& cb) {
    if (cb.pData && cb.iDataLen > 0) {
        return std::string(cb.pData, cb.iDataLen);
    }
    return "";
}

// Helper function to convert Python string to tsNCharcb
tsNCharcb string_to_tsNCharcb(const std::string& str) {
    tsNCharcb cb;
    cb.iDataLen = str.length();
    cb.pData = new char[cb.iDataLen + 1];
    std::memcpy(cb.pData, str.c_str(), cb.iDataLen);
    cb.pData[cb.iDataLen] = '\0';
    return cb;
}

// Python wrapper for AccountInfo
class PyAccountInfo {
public:
    std::string fcmId;
    std::string ibId;
    std::string accountId;
    std::string accountName;
    int creationSsboe;
    int creationUsecs;

    PyAccountInfo() = default;
    
    // Convert from C++ AccountInfo to Python
    PyAccountInfo(const AccountInfo& acc) {
        fcmId = tsNCharcb_to_string(acc.sFcmId);
        ibId = tsNCharcb_to_string(acc.sIbId);
        accountId = tsNCharcb_to_string(acc.sAccountId);
        accountName = tsNCharcb_to_string(acc.sAccountName);
        creationSsboe = acc.iCreationSsboe;
        creationUsecs = acc.iCreationUsecs;
    }

    // Convert to C++ AccountInfo
    AccountInfo to_cpp() const {
        AccountInfo acc;
        acc.sFcmId = string_to_tsNCharcb(fcmId);
        acc.sIbId = string_to_tsNCharcb(ibId);
        acc.sAccountId = string_to_tsNCharcb(accountId);
        acc.sAccountName = string_to_tsNCharcb(accountName);
        acc.iCreationSsboe = creationSsboe;
        acc.iCreationUsecs = creationUsecs;
        return acc;
    }
};

// Python wrapper for OrderData
class PyOrderData {
public:
    std::string order_id;
    std::string account_id;
    std::string symbol;
    std::string exchange;
    std::string side;
    std::string order_type;
    std::string status;
    long long quantity;
    long long filled_quantity;
    double price;
    double commission;
    int timestamp;

    PyOrderData() = default;
};

// Python callback handler class
class PyCallbacks : public RCallbacks {
public:
    py::function on_account_list;
    py::function on_order_replay;
    py::function on_order_history_dates;
    py::function on_product_rms_list;
    py::function on_alert;
    LoginCompletionHandler* login_handler;
    AccountListHandler* account_handler;  // Change to use interface

    void set_login_handler(LoginCompletionHandler* handler) {
        login_handler = handler;
    }

    void set_account_handler(AccountListHandler* handler) {
        account_handler = handler;
    }

    virtual int AccountList(AccountListInfo* pInfo, void* pContext, int* aiCode) override {
        if (!pInfo) {
            std::cout << "Account list callback received null info" << std::endl;
            *aiCode = API_IGNORED;
            return NOT_OK;
        }

        // Check response code
        if (pInfo->iRpCode != 0) {  // Non-zero means error
            std::cout << "Account list error: " << tsNCharcb_to_string(pInfo->sRpCode) 
                      << " (code: " << pInfo->iRpCode << ")" << std::endl;
            *aiCode = pInfo->iRpCode;
            return NOT_OK;
        }

        std::cout << "Account list callback received with " << pInfo->iArrayLen << " accounts" << std::endl;

        if (on_account_list) {
            try {
                std::vector<PyAccountInfo> accounts;
                for (int i = 0; i < pInfo->iArrayLen; i++) {
                    accounts.emplace_back(pInfo->asAccountInfoArray[i]);
                }
                on_account_list(accounts);
                
                // Notify that accounts were received using the interface
                if (account_handler) {
                    account_handler->on_account_list_received();
                }
                
                *aiCode = API_OK;
                return OK;
            } catch (const std::exception& e) {
                std::cout << "Error in account list callback: " << e.what() << std::endl;
                *aiCode = API_IGNORED;
                return NOT_OK;
            } catch (...) {
                std::cout << "Unknown error in account list callback" << std::endl;
                *aiCode = API_IGNORED;
                return NOT_OK;
            }
        }
        
        // Even if no callback is set, still notify that accounts were received
        if (account_handler) {
            account_handler->on_account_list_received();
        }
        
        *aiCode = API_OK;
        return OK;
    }

    virtual int OrderReplay(OrderReplayInfo* pInfo, void* pContext, int* aiCode) override {
        if (on_order_replay && pInfo) {
            std::vector<PyOrderData> orders;
            for (int i = 0; i < pInfo->iArrayLen; i++) {
                const auto& line = pInfo->asLineInfoArray[i];
                PyOrderData order;
                order.order_id = tsNCharcb_to_string(line.sOrderNum);
                order.account_id = tsNCharcb_to_string(line.oAccount.sAccountId);
                order.symbol = tsNCharcb_to_string(line.sTicker);
                order.exchange = tsNCharcb_to_string(line.sExchange);
                order.side = tsNCharcb_to_string(line.sBuySellType);
                order.order_type = tsNCharcb_to_string(line.sOrderType);
                order.status = tsNCharcb_to_string(line.sStatus);
                order.quantity = line.llQuantityToFill;
                order.filled_quantity = line.llFilled;
                order.price = line.dPriceToFill;
                order.timestamp = line.iSsboe;
                orders.push_back(order);
            }
            on_order_replay(orders);
        }
        *aiCode = API_OK;
        return OK;
    }

    virtual int OrderHistoryDates(OrderHistoryDatesInfo* pInfo, void* pContext, int* aiCode) override {
        if (on_order_history_dates && pInfo) {
            std::vector<std::string> dates;
            for (int i = 0; i < pInfo->iArrayLen; i++) {
                dates.push_back(tsNCharcb_to_string(pInfo->asDateArray[i]));
            }
            on_order_history_dates(dates);
        }
        *aiCode = API_OK;
        return OK;
    }

    virtual int ProductRmsList(ProductRmsListInfo* pInfo, void* pContext, int* aiCode) override {
        if (on_product_rms_list && pInfo) {
            std::map<std::string, double> commission_rates;
            for (int i = 0; i < pInfo->iArrayLen; i++) {
                const auto& rmsInfo = pInfo->asProductRmsInfoArray[i];
                if (rmsInfo.bCommissionFillRate) {
                    std::string product_code = tsNCharcb_to_string(rmsInfo.sProductCode);
                    commission_rates[product_code] = rmsInfo.dCommissionFillRate;
                }
            }
            on_product_rms_list(commission_rates);
        }
        *aiCode = API_OK;
        return OK;
    }

    virtual int Alert(AlertInfo* pInfo, void* pContext, int* aiCode) override {
        if (!pInfo) {
            *aiCode = API_IGNORED;
            return NOT_OK;
        }

        // Check for login completion using the interface
        if (login_handler) {
            login_handler->on_login_complete(pInfo->iAlertType, pInfo->iConnectionId);
        }

        if (on_alert) {
            try {
                on_alert(pInfo->iAlertType, tsNCharcb_to_string(pInfo->sMessage));
                *aiCode = API_OK;
                return OK;
            } catch (...) {
                *aiCode = API_IGNORED;
                return NOT_OK;
            }
        }
        *aiCode = API_OK;
        return OK;
    }
};

// Add missing types
class PyCommissionRate {
public:
    double rate;
    bool is_valid;

    PyCommissionRate() : rate(0.0), is_valid(false) {}
    PyCommissionRate(double r, bool v) : rate(r), is_valid(v) {}
};

class PyProcessingStats {
public:
    int total_days;
    int days_processed;
    int orders_processed;

    PyProcessingStats() : total_days(0), days_processed(0), orders_processed(0) {}
    PyProcessingStats(int total, int processed, int orders) 
        : total_days(total), days_processed(processed), orders_processed(orders) {}
};

// Add PyAdmCallbacks class
class PyAdmCallbacks : public AdmCallbacks {
public:
    virtual int Alert(AlertInfo* pInfo, void* pContext, int* aiCode) override {
        *aiCode = API_OK;
        return OK;
    }
};

// Add ConnectionParams class to handle Rithmic configuration
class ConnectionParams {
public:
    std::map<std::string, std::string> env_vars;
    std::string md_connect_point;
    std::string ts_connect_point;

    static ConnectionParams from_json(const std::string& json_str, const std::string& server_type, const std::string& location) {
        ConnectionParams params;
        try {
            // Parse JSON using nlohmann::json
            auto config_obj = nlohmann::json::parse(json_str);

            // Verify server type exists
            if (!config_obj.contains(server_type)) {
                throw std::runtime_error("Server type '" + server_type + "' not found in configuration");
            }

            // Verify server configs exist
            auto server_configs = config_obj[server_type]["server_configs"];
            if (!server_configs.contains(location)) {
                throw std::runtime_error("Location '" + location + "' not found in " + server_type + " configuration");
            }

            // Get the selected configuration
            auto selected_config = server_configs[location];

            // Verify all required fields are present
            if (!selected_config.contains("MML_DMN_SRVR_ADDR") || 
                !selected_config.contains("MML_DOMAIN_NAME") || 
                !selected_config.contains("MML_LIC_SRVR_ADDR") || 
                !selected_config.contains("MML_LOC_BROK_ADDR") || 
                !selected_config.contains("MML_LOGGER_ADDR") ||
                !selected_config.contains("MD_CNNCT_PT") ||
                !selected_config.contains("TS_CNNCT_PT")) {
                throw std::runtime_error("Missing required configuration fields for " + server_type + "/" + location);
            }

            // Set up environment variables from server configuration
            params.env_vars = {
                {"MML_LOG_TYPE", "log_net"},
                {"MML_SSL_CLNT_AUTH_FILE", "/app/bin/rithmic_ssl_cert_auth_params"},
                {"MML_DMN_SRVR_ADDR", selected_config["MML_DMN_SRVR_ADDR"].get<std::string>()},
                {"MML_DOMAIN_NAME", selected_config["MML_DOMAIN_NAME"].get<std::string>()},
                {"MML_LIC_SRVR_ADDR", selected_config["MML_LIC_SRVR_ADDR"].get<std::string>()},
                {"MML_LOC_BROK_ADDR", selected_config["MML_LOC_BROK_ADDR"].get<std::string>()},
                {"MML_LOGGER_ADDR", selected_config["MML_LOGGER_ADDR"].get<std::string>()},
                {"USER", "default_user"}  // Will be overridden during login
            };

            // Get connection points
            params.md_connect_point = selected_config["MD_CNNCT_PT"].get<std::string>();
            params.ts_connect_point = selected_config["TS_CNNCT_PT"].get<std::string>();

            // Log the configuration being used
            std::cout << "Using " << server_type << "/" << location << " configuration" << std::endl;
            std::cout << "MD Connect Point: " << params.md_connect_point << std::endl;
            std::cout << "TS Connect Point: " << params.ts_connect_point << std::endl;

        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to parse server configuration: " + std::string(e.what()));
        }
        return params;
    }

    std::vector<char*> to_env_array() const {
        std::vector<char*> result;
        for (const auto& [key, value] : env_vars) {
            std::string env_var = key + "=" + value;
            result.push_back(strdup(env_var.c_str()));
        }
        result.push_back(nullptr);  // Null terminate
        return result;
    }
};

// Enhanced PyREngine class
class PyREngine : public LoginCompletionHandler, public AccountListHandler {
private:
    REngine* engine;
    std::unique_ptr<PyCallbacks> callbacks;
    std::unique_ptr<PyAdmCallbacks> adm_callbacks;
    std::vector<char*> env_vars;
    ConnectionParams connection_params;
    int last_error_code = 0;
    bool ts_login_complete = false;
    bool ts_connection_opened = false;  // Add flag for connection opened
    bool received_account_list = false;
    std::mutex login_mutex;
    std::mutex account_mutex;
    std::condition_variable login_cv;
    std::condition_variable account_cv;

public:
    PyREngine(const std::string& appName, const std::string& appVersion, const std::string& config_json = "", const std::string& server_type = "", const std::string& location = "") {
        // Set up callbacks
        adm_callbacks = std::make_unique<PyAdmCallbacks>();
        callbacks = std::make_unique<PyCallbacks>();
        callbacks->set_login_handler(this);
        callbacks->set_account_handler(this);  // Set account handler

        // Validate app name and version (must contain at least one alphanumeric)
        if (!std::any_of(appName.begin(), appName.end(), ::isalnum)) {
            throw std::runtime_error("Application name must contain at least one alphanumeric character");
        }
        if (!std::any_of(appVersion.begin(), appVersion.end(), ::isalnum)) {
            throw std::runtime_error("Application version must contain at least one alphanumeric character");
        }

        // Load connection parameters
        try {
            std::string use_server_type = server_type;
            std::string use_location = location;

            // If server type and location are not provided, try to find them in the config
            if (use_server_type.empty() || use_location.empty()) {
                // Parse the config JSON to get server type and location
                auto config = nlohmann::json::parse(config_json);
                
                // Find the first server type that has server_configs
                for (auto& [type, data] : config.items()) {
                    if (data.contains("server_configs") && !data["server_configs"].empty()) {
                        use_server_type = type;
                        use_location = data["server_configs"].begin().key();
                        break;
                    }
                }
                
                if (use_server_type.empty() || use_location.empty()) {
                    throw std::runtime_error("Could not find valid server type and location in configuration");
                }
            }

            connection_params = ConnectionParams::from_json(config_json, use_server_type, use_location);
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to load connection parameters: " + std::string(e.what()));
        }

        // Set up environment variables from connection parameters
        env_vars = connection_params.to_env_array();

        // Create initial parameters with proper initialization
        REngineParams params;
        std::memset(&params, 0, sizeof(REngineParams)); // Zero-initialize the structure

        // Set up application info (must contain alphanumeric)
        params.sAppName.pData = strdup(appName.c_str());
        params.sAppName.iDataLen = appName.length();
        params.sAppVersion.pData = strdup(appVersion.c_str());
        params.sAppVersion.iDataLen = appVersion.length();

        // Set up logging (optional but helpful for debugging)
        params.sLogFilePath.pData = strdup("/tmp/rithmic.log");
        params.sLogFilePath.iDataLen = strlen(params.sLogFilePath.pData);

        // Set up required callbacks and environment
        params.pAdmCallbacks = adm_callbacks.get();  // Required
        params.envp = env_vars.data();  // Required
        params.pContext = nullptr;  // Optional
        
        try {
            // Create the REngine instance
            engine = new REngine(&params);

            // Clean up allocated strings
            free((void*)params.sAppName.pData);
            free((void*)params.sAppVersion.pData);
            free((void*)params.sLogFilePath.pData);

        } catch (const OmneException& e) {
            // Clean up allocated strings
            free((void*)params.sAppName.pData);
            free((void*)params.sAppVersion.pData);
            free((void*)params.sLogFilePath.pData);

            // Clean up environment variables
            for (char* env : env_vars) {
                if (env) free(env);
            }
            
            // Log error but don't throw from destructor
            OmneException non_const_e = e;  // Create non-const copy
            std::cout << "Error during REngine cleanup: " << non_const_e.getErrorString() << std::endl;
        }
    }

    ~PyREngine() {
        try {
            if (engine) {
                // Ensure proper logout before destruction
                int code;
                engine->logout(&code);
                delete engine;
                engine = nullptr;
            }
        } catch (const OmneException& e) {
            // Log error but don't throw from destructor
            OmneException non_const_e = e;  // Create non-const copy
            std::cout << "Error during REngine cleanup: " << non_const_e.getErrorString() << std::endl;
        }

        // Clean up environment variables
        for (char* env : env_vars) {
            if (env) free(env);
        }
    }

    void set_callbacks(
        py::object on_account_list,
        py::object on_order_replay,
        py::object on_order_history_dates,
        py::object on_product_rms_list,
        py::object on_alert
    ) {
        callbacks->on_account_list = on_account_list.is_none() ? py::function() : on_account_list.cast<py::function>();
        callbacks->on_order_replay = on_order_replay.is_none() ? py::function() : on_order_replay.cast<py::function>();
        callbacks->on_order_history_dates = on_order_history_dates.is_none() ? py::function() : on_order_history_dates.cast<py::function>();
        callbacks->on_product_rms_list = on_product_rms_list.is_none() ? py::function() : on_product_rms_list.cast<py::function>();
        callbacks->on_alert = on_alert.is_none() ? py::function() : on_alert.cast<py::function>();
    }

    bool login(const std::string& user, const std::string& password) {
        // Reset login flags
        ts_login_complete = false;
        ts_connection_opened = false;

        // Validate username and password
        if (user.empty() || password.empty()) {
            std::cout << "Login failed: username or password is empty" << std::endl;
            last_error_code = NOT_OK;
            return false;
        }

        // Username should be alphanumeric and not too long
        if (user.length() > 50 || !std::all_of(user.begin(), user.end(), [](char c) { 
            return std::isalnum(c) || c == '_' || c == '-'; 
        })) {
            std::cout << "Login failed: username contains invalid characters or is too long" << std::endl;
            last_error_code = NOT_OK;
            return false;
        }

        std::cout << "Starting login process for user: " << user << std::endl;
        std::cout << "TS Connect Point: " << connection_params.ts_connect_point << std::endl;

        // First update the USER environment variable
        std::map<std::string, std::string> new_vars = {
            {"USER", user}  // Set the USER environment variable to match login username
        };
        update_env_vars(new_vars);

        // Now set up login parameters with proper initialization
        LoginParams params;
        std::memset(&params, 0, sizeof(LoginParams));  // Zero-initialize the structure

        // Set up the parameters
        auto ts_user = string_to_tsNCharcb(user);
        auto ts_password = string_to_tsNCharcb(password);
        auto ts_cnnct_pt = string_to_tsNCharcb(connection_params.ts_connect_point);

        // Set up trading system credentials
        params.sTsUser = ts_user;
        params.sTsPassword = ts_password;
        params.sTsCnnctPt = ts_cnnct_pt;

        // Set market data credentials to match trading system credentials
        params.sMdUser = ts_user;  // Use same credentials for market data
        params.sMdPassword = ts_password;
        params.sMdCnnctPt = string_to_tsNCharcb(connection_params.md_connect_point);

        // Set callbacks
        params.pCallbacks = callbacks.get();

        std::cout << "Initiating login with REngine..." << std::endl;
        int code = API_OK;
        int result = engine->login(&params, &code);
        last_error_code = code;

        // Clean up allocated memory
        delete[] ts_user.pData;
        delete[] ts_password.pData;
        delete[] ts_cnnct_pt.pData;
        delete[] params.sMdCnnctPt.pData;

        if (result == NOT_OK) {
            std::cout << "Login call failed with error code: " << code << std::endl;
            try {
                OmneException ex(code);
                std::cout << "Error message: " << ex.getErrorString() << std::endl;
            } catch (...) {
                std::cout << "Could not get error message for code: " << code << std::endl;
            }
            return false;
        }

        std::cout << "Login call successful, waiting for completion..." << std::endl;
        return wait_for_login();
    }

    bool logout() {
        int code;
        bool result = engine->logout(&code);
        last_error_code = code;  // Store the error code from the aiCode parameter
        return result;
    }

    bool get_accounts(const std::string& status = "") {
        // Make sure we're fully logged in first
        if (!ts_connection_opened || !ts_login_complete) {
            std::cout << "Not fully logged in. Connection opened: " << ts_connection_opened 
                      << ", Login complete: " << ts_login_complete << std::endl;
            return false;
        }

        // Add a retry mechanism for the "no handle" error
        int retries = 3;
        while (retries > 0) {
            std::cout << "Requesting account list (attempts remaining: " << retries << ")..." << std::endl;

            // Reset account list flag
            received_account_list = false;

            int code;
            tsNCharcb* status_param = nullptr;
            tsNCharcb status_cb;

            // If status is provided, validate and set it
            if (!status.empty()) {
                if (status != "active" && status != "inactive" && status != "admin only") {
                    std::cout << "Invalid status value. Must be 'active', 'inactive', or 'admin only'" << std::endl;
                    return false;
                }
                status_cb = string_to_tsNCharcb(status);
                status_param = &status_cb;
            }

            bool result = engine->getAccounts(status_param, &code);
            last_error_code = code;

            // Clean up if we allocated status
            if (status_param) {
                delete[] status_cb.pData;
            }

            if (result == NOT_OK) {
                if (code == 11 && retries > 1) { // "no handle" error
                    std::cout << "Connection not ready (error code: " << code << "), retrying in 1 second..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    retries--;
                    continue;
                }
                std::cout << "Failed to request accounts, error code: " << code << std::endl;
                return false;
            }

            std::cout << "Account list requested, waiting for response..." << std::endl;

            // Wait for account list to be received
            std::unique_lock<std::mutex> lock(account_mutex);
            if (!account_cv.wait_for(lock, std::chrono::seconds(30), [this] { 
                return received_account_list; 
            })) {
                std::cout << "Timeout waiting for account list" << std::endl;
                return false;
            }

            std::cout << "Account list received successfully" << std::endl;
            return true;
        }

        return false;
    }

    bool replay_all_orders(const PyAccountInfo& account, int start_ssboe, int end_ssboe) {
        int code;
        auto cpp_account = account.to_cpp();
        return engine->replayAllOrders(&cpp_account, start_ssboe, end_ssboe, &code);
    }

    bool replay_historical_orders(const PyAccountInfo& account, const std::string& date) {
        int code;
        auto cpp_account = account.to_cpp();
        auto cpp_date = string_to_tsNCharcb(date);
        return engine->replayHistoricalOrders(&cpp_account, &cpp_date, &code);
    }

    bool list_order_history_dates(const PyAccountInfo& account) {
        int code;
        return engine->listOrderHistoryDates(nullptr, &code);
    }

    bool get_product_rms_info(const PyAccountInfo& account) {
        int code;
        auto cpp_account = account.to_cpp();
        return engine->getProductRmsInfo(&cpp_account, &code);
    }

    bool subscribe_order(const PyAccountInfo& account) {
        int code;
        auto cpp_account = account.to_cpp();
        return engine->subscribeOrder(&cpp_account, &code);
    }

    bool unsubscribe_order(const PyAccountInfo& account) {
        int code;
        auto cpp_account = account.to_cpp();
        return engine->unsubscribeOrder(&cpp_account, &code);
    }

    // Add method to get error code
    int get_error_code() const {  // Make this const since it doesn't modify state
        return last_error_code;  // Simply return the stored error code
    }

    // Add method to get error string
    static std::string get_error_string(int error_code) {
        try {
            OmneException ex(error_code);
            return ex.getErrorString();
        } catch (...) {
            return "Unknown error";
        }
    }

    // Add method to get version
    static std::string get_version() {
        tsNCharcb version;
        int code;
        if (REngine::getVersion(&version, &code)) {
            return tsNCharcb_to_string(version);
        }
        return "";
    }

    // Modify update_env_vars to only update environment variables
    void update_env_vars(const std::map<std::string, std::string>& new_vars) {
        // Clean up existing environment variables
        for (char* env : env_vars) {
            if (env) free(env);
        }
        env_vars.clear();

        // Required environment variables with default values
        std::map<std::string, std::string> required_vars = {
            {"MML_LOG_TYPE", "log_net"},
            {"MML_SSL_CLNT_AUTH_FILE", "/app/bin/rithmic_ssl_cert_auth_params"},
            {"MML_DMN_SRVR_ADDR", connection_params.env_vars["MML_DMN_SRVR_ADDR"]},
            {"MML_DOMAIN_NAME", connection_params.env_vars["MML_DOMAIN_NAME"]},
            {"MML_LIC_SRVR_ADDR", connection_params.env_vars["MML_LIC_SRVR_ADDR"]},
            {"MML_LOC_BROK_ADDR", connection_params.env_vars["MML_LOC_BROK_ADDR"]},
            {"MML_LOGGER_ADDR", connection_params.env_vars["MML_LOGGER_ADDR"]},
            {"USER", "default_user"}
        };

        // Update required vars with any provided new values
        for (const auto& [key, value] : new_vars) {
            required_vars[key] = value;
        }

        // Add all environment variables
        for (const auto& [key, value] : required_vars) {
            std::string env_var = key + "=" + value;
            env_vars.push_back(strdup(env_var.c_str()));
        }

        // Add null terminator
        env_vars.push_back(nullptr);
    }

    // Implement the interface methods
    void on_login_complete(int alert_type, int connection_id) override {
        if (connection_id == TRADING_SYSTEM_CONNECTION_ID) {
            std::lock_guard<std::mutex> lock(login_mutex);
            if (alert_type == ALERT_CONNECTION_OPENED) {
                ts_connection_opened = true;
                std::cout << "Trading system connection opened (alert_type=" << alert_type << ")" << std::endl;
            } else if (alert_type == ALERT_LOGIN_COMPLETE) {
                ts_login_complete = true;
                std::cout << "Trading system login complete (alert_type=" << alert_type << ")" << std::endl;
            } else if (alert_type == ALERT_LOGIN_FAILED) {
                std::cout << "Trading system login failed (alert_type=" << alert_type << ")" << std::endl;
            }
            std::cout << "Current login state - Connection opened: " << ts_connection_opened 
                      << ", Login complete: " << ts_login_complete << std::endl;
            login_cv.notify_one();
        }
    }

    bool wait_for_login() {
        std::unique_lock<std::mutex> lock(login_mutex);
        std::cout << "Waiting for login completion..." << std::endl;
        // Wait for both connection opened and login complete
        if (!login_cv.wait_for(lock, std::chrono::seconds(30), [this] { 
            return ts_connection_opened && ts_login_complete; 
        })) {
            std::cout << "Login timeout. Connection opened: " << ts_connection_opened 
                      << ", Login complete: " << ts_login_complete << std::endl;
            return false;
        }
        std::cout << "Login completed successfully" << std::endl;
        
        // Add a small delay after login completion to ensure the connection is ready
        std::cout << "Waiting for connection to be ready..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        return true;
    }

    void on_account_list_received() override {
        std::lock_guard<std::mutex> lock(account_mutex);
        received_account_list = true;
        account_cv.notify_one();
    }
};

// Add missing constants to the module
void add_constants(py::module& m) {
    // Order types
    m.attr("ORDER_TYPE_LIMIT") = py::str(sORDER_TYPE_LIMIT.pData);
    m.attr("ORDER_TYPE_MARKET") = py::str(sORDER_TYPE_MARKET.pData);
    m.attr("ORDER_TYPE_STOP_MARKET") = py::str(sORDER_TYPE_STOP_MARKET.pData);
    m.attr("ORDER_TYPE_STOP_LIMIT") = py::str(sORDER_TYPE_STOP_LIMIT.pData);

    // Buy/Sell types
    m.attr("BUY_SELL_TYPE_BUY") = py::str(sBUY_SELL_TYPE_BUY.pData);
    m.attr("BUY_SELL_TYPE_SELL") = py::str(sBUY_SELL_TYPE_SELL.pData);
    m.attr("BUY_SELL_TYPE_SELL_SHORT") = py::str(sBUY_SELL_TYPE_SELL_SHORT.pData);

    // Order duration
    m.attr("ORDER_DURATION_DAY") = py::str(sORDER_DURATION_DAY.pData);
    m.attr("ORDER_DURATION_GTC") = py::str(sORDER_DURATION_GTC.pData);

    // Line status
    m.attr("LINE_STATUS_OPEN") = py::str(sLINE_STATUS_OPEN.pData);
    m.attr("LINE_STATUS_COMPLETE") = py::str(sLINE_STATUS_COMPLETE.pData);
    m.attr("LINE_STATUS_CANCEL_PENDING") = py::str(sLINE_STATUS_CANCEL_PENDING.pData);
    m.attr("LINE_STATUS_MODIFY_PENDING") = py::str(sLINE_STATUS_MODIFY_PENDING.pData);

    // Return codes
    m.attr("OK") = OK;
    m.attr("BAD") = NOT_OK;
    m.attr("API_OK") = API_OK;
}

PYBIND11_MODULE(rapi, m) {
    m.doc() = "Python bindings for RApiPlus library";

    // Expose AccountInfo
    py::class_<PyAccountInfo>(m, "AccountInfo")
        .def(py::init<>())
        .def_readwrite("fcm_id", &PyAccountInfo::fcmId)
        .def_readwrite("ib_id", &PyAccountInfo::ibId)
        .def_readwrite("account_id", &PyAccountInfo::accountId)
        .def_readwrite("account_name", &PyAccountInfo::accountName)
        .def_readwrite("creation_ssboe", &PyAccountInfo::creationSsboe)
        .def_readwrite("creation_usecs", &PyAccountInfo::creationUsecs);

    // Expose OrderData
    py::class_<PyOrderData>(m, "OrderData")
        .def(py::init<>())
        .def_readwrite("order_id", &PyOrderData::order_id)
        .def_readwrite("account_id", &PyOrderData::account_id)
        .def_readwrite("symbol", &PyOrderData::symbol)
        .def_readwrite("exchange", &PyOrderData::exchange)
        .def_readwrite("side", &PyOrderData::side)
        .def_readwrite("order_type", &PyOrderData::order_type)
        .def_readwrite("status", &PyOrderData::status)
        .def_readwrite("quantity", &PyOrderData::quantity)
        .def_readwrite("filled_quantity", &PyOrderData::filled_quantity)
        .def_readwrite("price", &PyOrderData::price)
        .def_readwrite("commission", &PyOrderData::commission)
        .def_readwrite("timestamp", &PyOrderData::timestamp);

    // Expose CommissionRate
    py::class_<PyCommissionRate>(m, "CommissionRate")
        .def(py::init<>())
        .def(py::init<double, bool>())
        .def_readwrite("rate", &PyCommissionRate::rate)
        .def_readwrite("is_valid", &PyCommissionRate::is_valid);

    // Expose ProcessingStats
    py::class_<PyProcessingStats>(m, "ProcessingStats")
        .def(py::init<>())
        .def(py::init<int, int, int>())
        .def_readwrite("total_days", &PyProcessingStats::total_days)
        .def_readwrite("days_processed", &PyProcessingStats::days_processed)
        .def_readwrite("orders_processed", &PyProcessingStats::orders_processed);

    // Expose REngine
    py::class_<PyREngine>(m, "REngine")
        .def(py::init<const std::string&, const std::string&, const std::string&, const std::string&, const std::string&>(),
            py::arg("app_name"),
            py::arg("app_version"),
            py::arg("config_json") = "",
            py::arg("server_type") = "",
            py::arg("location") = "")
        .def("set_callbacks", &PyREngine::set_callbacks)
        .def("login", &PyREngine::login)
        .def("logout", &PyREngine::logout)
        .def("get_accounts", &PyREngine::get_accounts,
            py::arg("status") = "",
            "Get accounts for the currently logged in user. Optional status parameter can be 'active', 'inactive', or 'admin only'.")
        .def("replay_all_orders", &PyREngine::replay_all_orders)
        .def("replay_historical_orders", &PyREngine::replay_historical_orders)
        .def("list_order_history_dates", &PyREngine::list_order_history_dates)
        .def("get_product_rms_info", &PyREngine::get_product_rms_info)
        .def("subscribe_order", &PyREngine::subscribe_order)
        .def("unsubscribe_order", &PyREngine::unsubscribe_order)
        .def("get_error_code", &PyREngine::get_error_code)
        .def_static("get_error_string", &PyREngine::get_error_string)
        .def_static("get_version", &PyREngine::get_version)
        .def("update_env_vars", &PyREngine::update_env_vars);

    // Add constants
    m.attr("ALERT_CONNECTION_OPENED") = ALERT_CONNECTION_OPENED;
    m.attr("ALERT_CONNECTION_CLOSED") = ALERT_CONNECTION_CLOSED;
    m.attr("ALERT_LOGIN_COMPLETE") = ALERT_LOGIN_COMPLETE;
    m.attr("ALERT_LOGIN_FAILED") = ALERT_LOGIN_FAILED;
    
    m.attr("MARKET_DATA_CONNECTION_ID") = MARKET_DATA_CONNECTION_ID;
    m.attr("TRADING_SYSTEM_CONNECTION_ID") = TRADING_SYSTEM_CONNECTION_ID;
    m.attr("PNL_CONNECTION_ID") = PNL_CONNECTION_ID;

    // Add all constants
    add_constants(m);
} 