#include "RApiPlus.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <sstream>
#include <vector>
#include <iostream>
#include <algorithm>
#include <utility>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <unordered_map>
#include <fstream>
#include <ctime>
#include <set>
#include <sys/stat.h>
#include <chrono>
#include <cmath>
#include <json.hpp>

using json = nlohmann::json;

// Add the OrderData struct definition here
struct OrderData {
    std::string order_id;
    std::string account_id;
    std::string symbol;
    std::string exchange;
    std::string side;  // buy/sell
    std::string order_type;
    std::string status;
    long long quantity;
    long long filled_quantity;
    double price;
    double commission;
    int timestamp;
};

#define GOOD 0
#define BAD  1

// Helper function to escape JSON strings
std::string escape_json(const char* str) {
    std::string result;
    while (*str) {
        switch (*str) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            default: result += *str;
        }
        str++;
    }
    return result;
}

// Helper function to send log messages in JSON format
void send_log(const char* level, const std::string& message) {
    // Replace any newlines with spaces to keep the JSON on one line
    std::string cleaned_message = message;
    std::replace(cleaned_message.begin(), cleaned_message.end(), '\n', ' ');
    
    // Escape the cleaned message
    std::string escaped_message = escape_json(cleaned_message.c_str());
    
    fprintf(stdout, "{\"type\":\"log\",\"level\":\"%s\",\"message\":\"%s\"}\n", 
        level, escaped_message.c_str());
    fflush(stdout);
}

// Variadic template to handle multiple arguments
template<typename... Args>
void debug_print(const char* format, Args... args) {
    char buffer[2048];
    snprintf(buffer, sizeof(buffer), format, args...);
    send_log("info", buffer);
}

template<typename... Args>
void error_print(const char* format, Args... args) {
    char buffer[2048];
    snprintf(buffer, sizeof(buffer), format, args...);
    send_log("error", buffer);
}

#define DEBUG_PRINT(...) debug_print(__VA_ARGS__)
#define ERROR_PRINT(...) error_print(__VA_ARGS__)

using namespace RApi;

/*   =====================================================================   */
/*   Use global variables to share between the callback thread and main      */
/*   thread.  The booleans are a primitive method of signaling state         */
/*   between the two threads.                                                */

// Add forward declaration before globals
class JsonOrderWriter;

bool        g_bTsLoginComplete = false;
bool        g_bRcvdAccount     = false;
bool        g_bRcvdPriceIncr   = false;
bool        g_bRcvdTradeRoutes = false;
bool        g_bDone            = false;
int         g_max_days         = 300;  // Default value for max days to process
std::string g_start_date;              // Start date in YYYYMMDD format

std::vector<OrderData> g_OrderDataList;

int         g_iToExchSsboe     = 0;
int         g_iToExchUsecs     =0;
int         g_iFromExchSsboe   =0;
int         g_iFromExchUsecs   =0;

const int   g_iMAX_LEN         =256;
char        g_cAccountId[g_iMAX_LEN];
char        g_cFcmId[g_iMAX_LEN];
char        g_cIbId[g_iMAX_LEN];
char        g_cExchange[g_iMAX_LEN];
char        g_cTradeRoute[g_iMAX_LEN];
AccountInfo g_oAccount;
tsNCharcb   g_sExchange;
tsNCharcb   g_sTradeRoute      = {(char *)NULL, 0};

REngine *   g_pEngine;

bool        g_bRcvdOrderReplay    = false;
int         g_iNumOrdersReceived  =0;
tsNCharcb*  g_OrderIds           = NULL; // Array to store order IDs

bool g_bRcvdHistoryDates = false;
int g_iNumDatesReceived =0;
tsNCharcb* g_HistoryDates = NULL;

const int MAX_CONCURRENT_REQUESTS = 10;  // Increased from 3 to 10 for better parallelization
std::vector<bool> g_DateProcessed;  // Track which dates have been processed
int g_OutstandingRequests =0;  // Track number of outstanding requests

std::vector<AccountInfo> g_AccountList;
bool g_bProcessingAccounts = false;
size_t g_CurrentAccountIndex =0;

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_running = true;
std::vector<int> g_websocket_clients;

// Replace the existing global commission rates map with this structure
struct CommissionRate {
    double rate;
    bool is_valid;
};

// Use an unordered_map for faster lookups
std::unordered_map<std::string, CommissionRate> g_commission_rates;

// Add a map to store orders by account
std::map<std::string, std::vector<OrderData> > g_AccountOrdersMap;

// Add at the start of the file with other globals
struct ProcessingStats {
    ProcessingStats(int total =0, int processed =0, int orders =0) 
        : total_days(total), days_processed(processed), orders_processed(orders) {}
    int total_days;
    int days_processed;
    int orders_processed;
};


std::map<std::string, ProcessingStats> g_AccountStats;

std::set<std::string> g_RequestedAccounts;

// Add these global variables at the start with other globals
JsonOrderWriter* g_writer = nullptr;
std::mutex g_writer_mutex;

// Add after the other global variables
std::ofstream g_log_file;

// Add after other helper functions
void log_to_file(const std::string& level, const std::string& message) {
    if (!g_log_file.is_open()) {
        g_log_file.open("order_fetcher_debug.log", std::ios::app);
    }
    if (g_log_file.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto now_time = std::chrono::system_clock::to_time_t(now);
        g_log_file << std::ctime(&now_time) << " [" << level << "] " << message << std::endl;
        g_log_file.flush();
    }
}

int main(int argc, char** argv, char** envp);

// Add after other global variables at the top
std::string g_orders_filename;

/*   =====================================================================   */
/*                          class declarations                               */
/*   =====================================================================   */

class MyAdmCallbacks: public AdmCallbacks
     {
     public :
     MyAdmCallbacks()  {};
     ~MyAdmCallbacks() {};

     /*   ----------------------------------------------------------------   */

     virtual int Alert(AlertInfo * pInfo,
                       void *      pContext,
                       int *       aiCode);
     };

/*   =====================================================================   */

class MyCallbacks: public RCallbacks
{
public:
    MyCallbacks()  {};
    ~MyCallbacks() {};

    virtual int Alert(AlertInfo * pInfo,
                     void *      pContext,
                     int *       aiCode);

    virtual int AccountList(AccountListInfo * pInfo,
                          void *            pContext,
                          int *             aiCode);

    virtual int PasswordChange(PasswordChangeInfo * pInfo,
                             void *               pContext,
                             int *                aiCode);

    virtual int ExchangeList(ExchangeListInfo * pInfo,
                           void *             pContext,
                           int *              aiCode);

    virtual int ExecutionReplay(ExecutionReplayInfo * pInfo,
                              void *                pContext,
                              int *                 aiCode);

    virtual int LineUpdate(LineInfo * pInfo,
                         void *     pContext,
                         int *      aiCode);

    virtual int OpenOrderReplay(OrderReplayInfo * pInfo,
                              void *            pContext,
                              int *             aiCode);

    virtual int OrderReplay(OrderReplayInfo * pInfo,
                          void *            pContext,
                          int *             aiCode);

    virtual int PnlReplay(PnlReplayInfo * pInfo,
                        void *          pContext,
                        int *           aiCode);

    virtual int PnlUpdate(PnlInfo * pInfo,
                        void *    pContext,
                        int *     aiCode);

    virtual int SingleOrderReplay(SingleOrderReplayInfo * pInfo,
                                void *                  pContext,
                                int *                   aiCode);

    virtual int OrderHistoryDates(OrderHistoryDatesInfo* pInfo,
                            void* pContext,
                            int* aiCode);

    virtual int ProductRmsList(ProductRmsListInfo* pInfo, void* pContext, int* aiCode);

private:
};

/*   =====================================================================   */

int MyAdmCallbacks::Alert(AlertInfo * pInfo,
                          void *      pContext,
                          int *       aiCode)
     {
     int iIgnored;

     /*   ----------------------------------------------------------------   */

     DEBUG_PRINT("\n\n");
     if (!pInfo -> dump(&iIgnored))
          {
          DEBUG_PRINT("error in pInfo -> dump : %d", iIgnored);
          }

     /*   ----------------------------------------------------------------   */

     *aiCode = API_OK;
     return (OK);
     }

/*   =====================================================================   */

// Helper function to safely print account info
void print_account_info(const char* prefix, const AccountInfo& account) {
    if (account.sAccountId.pData && account.sAccountId.iDataLen > 0) {
        std::string account_id(account.sAccountId.pData, account.sAccountId.iDataLen);
        fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"%s: %s\"}\n", 
            prefix, account_id.c_str());
        fflush(stderr);
    }
}

// Add these helper functions at the top after includes
bool isValidString(const tsNCharcb* str) {
    if (!str) return false;
    if (!str->pData) return false;
    if (str->iDataLen <= 0 || str->iDataLen >= 1024) return false;
    // Verify the memory is readable
    try {
        volatile char test = str->pData[0];
        volatile char test2 = str->pData[str->iDataLen - 1];
        (void)test;
        (void)test2;
    } catch (...) {
        return false;
    }
    return true;
}

bool isValidAccount(const AccountInfo* account) {
    if (!account) return false;
    try {
        return isValidString(&account->sAccountId) &&
               isValidString(&account->sFcmId) &&
               isValidString(&account->sIbId);
    } catch (...) {
        return false;
    }
}

void safeDeleteString(tsNCharcb& str) {
    if (str.pData) {
        try {
            delete[] str.pData;
        } catch (...) {
            // Log error but don't throw
            fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Error deleting string memory\"}\n");
        }
        str.pData = nullptr;
        str.iDataLen =0;
    }
}

// Add safe string copy function
bool safeCopyString(tsNCharcb& dest, const tsNCharcb& src) {
    if (!isValidString(&src)) return false;
    
    try {
        dest.pData = new char[src.iDataLen + 1];
        memcpy(dest.pData, src.pData, src.iDataLen);
        dest.pData[src.iDataLen] = '\0';
        dest.iDataLen = src.iDataLen;
        return true;
    } catch (...) {
        if (dest.pData) {
            delete[] dest.pData;
            dest.pData = nullptr;
        }
        dest.iDataLen =0;
        return false;
    }
}

// Add safe cleanup function
void cleanupResources() {
    // Clean up account list with safety checks
    for (auto& account : g_AccountList) {
        if (account.sAccountId.pData) {
            delete[] account.sAccountId.pData;
            account.sAccountId.pData = nullptr;
        }
        if (account.sFcmId.pData) {
            delete[] account.sFcmId.pData;
            account.sFcmId.pData = nullptr;
        }
        if (account.sIbId.pData) {
            delete[] account.sIbId.pData;
            account.sIbId.pData = nullptr;
        }
    }
    g_AccountList.clear();

    // Clean up history dates with safety checks
    if (g_HistoryDates) {
        for (int i = 0; i < g_iNumDatesReceived; i++) {
            if (g_HistoryDates[i].pData) {
                delete[] g_HistoryDates[i].pData;
                g_HistoryDates[i].pData = nullptr;
            }
        }
        delete[] g_HistoryDates;
        g_HistoryDates = nullptr;
    }
    g_iNumDatesReceived = 0;

    // Clean up writer with safety check
    {
        std::lock_guard<std::mutex> lock(g_writer_mutex);
        if (g_writer) {
            delete g_writer;
            g_writer = nullptr;
        }
    }

    // Clean up global account
    if (g_oAccount.sAccountId.pData) {
        delete[] g_oAccount.sAccountId.pData;
        g_oAccount.sAccountId.pData = nullptr;
    }
    if (g_oAccount.sFcmId.pData) {
        delete[] g_oAccount.sFcmId.pData;
        g_oAccount.sFcmId.pData = nullptr;
    }
    if (g_oAccount.sIbId.pData) {
        delete[] g_oAccount.sIbId.pData;
        g_oAccount.sIbId.pData = nullptr;
    }

    // Clear other global data structures
    g_AccountOrdersMap.clear();
    g_AccountStats.clear();
    g_RequestedAccounts.clear();
    g_commission_rates.clear();
    g_DateProcessed.clear();
}

int MyCallbacks::AccountList(AccountListInfo* pInfo, void* pContext, int* aiCode) {
    if (!pInfo || !aiCode) {
        fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Invalid parameters in AccountList callback\"}\n");
        return BAD;
    }

    *aiCode = API_OK;  // Set default return value

    try {
        if (pInfo->iArrayLen > 0 && pInfo->asAccountInfoArray) {
            fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Processing %d accounts\"}\n", 
                pInfo->iArrayLen);
            
            // Clear existing account list first
            for (auto& account : g_AccountList) {
                safeDeleteString(account.sAccountId);
                safeDeleteString(account.sFcmId);
                safeDeleteString(account.sIbId);
            }
            g_AccountList.clear();

            // Pre-allocate space to prevent reallocation issues
            try {
                g_AccountList.reserve(pInfo->iArrayLen);
            } catch (const std::exception& e) {
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Failed to reserve space for accounts: %s\"}\n",
                    e.what());
                return BAD;
            }

            for (int i = 0; i < pInfo->iArrayLen; i++) {
                const AccountInfo& srcAccount = pInfo->asAccountInfoArray[i];
                
                // Validate account data
                if (!isValidAccount(&srcAccount)) {
                    fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Invalid account data at index %d\"}\n", i);
                    continue;
                }
                
                try {
                    AccountInfo newAccount;
                    memset(&newAccount, 0, sizeof(AccountInfo));
                    
                    // Safely copy each string field
                    if (!safeCopyString(newAccount.sAccountId, srcAccount.sAccountId) ||
                        !safeCopyString(newAccount.sFcmId, srcAccount.sFcmId) ||
                        !safeCopyString(newAccount.sIbId, srcAccount.sIbId)) {
                        fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Failed to copy account strings at index %d\"}\n", i);
                        continue;
                    }
                    
                    // Add to global list
                    g_AccountList.push_back(newAccount);
                    
                    // Log success
                    if (isValidString(&newAccount.sAccountId)) {
                        fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Successfully added account: %.*s\"}\n",
                            newAccount.sAccountId.iDataLen, newAccount.sAccountId.pData);
                    }
                } catch (const std::exception& e) {
                    fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Exception processing account at index %d: %s\"}\n",
                        i, e.what());
                    continue;
                }
            }
            
            // Store the first account in g_oAccount for backward compatibility
            if (!g_AccountList.empty()) {
                try {
                    memset(&g_oAccount, 0, sizeof(AccountInfo));
                    const AccountInfo& firstAccount = g_AccountList[0];
                    
                    if (!safeCopyString(g_oAccount.sAccountId, firstAccount.sAccountId) ||
                        !safeCopyString(g_oAccount.sFcmId, firstAccount.sFcmId) ||
                        !safeCopyString(g_oAccount.sIbId, firstAccount.sIbId)) {
                        fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Failed to copy first account to g_oAccount\"}\n");
                    }
                } catch (const std::exception& e) {
                    fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Exception copying first account: %s\"}\n",
                        e.what());
                }
            }

            // Verify account list integrity
            fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Successfully processed %zu accounts\"}\n",
                g_AccountList.size());
        } else {
            fprintf(stderr, "{\"type\":\"log\",\"level\":\"warning\",\"message\":\"No accounts received in AccountList callback\"}\n");
        }
        
        g_bRcvdAccount = true;
        return OK;
        
    } catch (const std::exception& e) {
        fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Critical exception in AccountList: %s\"}\n",
            e.what());
        g_bRcvdAccount = true;  // Set this to prevent hanging
        return BAD;
    } catch (...) {
        fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Unknown critical exception in AccountList\"}\n");
        g_bRcvdAccount = true;  // Set this to prevent hanging
        return BAD;
    }
}

/*   =====================================================================   */

int MyCallbacks::PasswordChange(PasswordChangeInfo * pInfo,
				void *               pContext,
				int *                aiCode)
     {
     *aiCode = API_OK;
     return (OK);
     }

/*   =====================================================================   */

int MyCallbacks::Alert(AlertInfo * pInfo,
                       void *      pContext,
                       int *       aiCode)
     {
    // Format alert message as JSON
    std::stringstream msg;
    if (pInfo->sMessage.pData && pInfo->sMessage.iDataLen > 0) {
        msg << std::string(pInfo->sMessage.pData, pInfo->sMessage.iDataLen);
    }
    
    fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"%s\"}\n", 
        msg.str().c_str());
    fflush(stderr);

    // Signal when the login to the trading system is complete
    if (pInfo->iAlertType == ALERT_LOGIN_COMPLETE &&
        pInfo->iConnectionId == TRADING_SYSTEM_CONNECTION_ID)
    {
        g_bTsLoginComplete = true;
    }

    *aiCode = API_OK;
    return (OK);
     }

/*   =====================================================================   */

int MyCallbacks::ExchangeList(ExchangeListInfo * pInfo,
			      void *             pContext,
			      int *              aiCode)
     {
     *aiCode = API_OK;
     return (OK);
     }

/*   =====================================================================   */

int MyCallbacks::ExecutionReplay(ExecutionReplayInfo * pInfo,
                                 void *                pContext,
                                 int *                 aiCode)
     {
     *aiCode = API_OK;
     return(OK);
     }


/*   =====================================================================   */

int MyCallbacks::LineUpdate(LineInfo * pInfo,
                            void *     pContext,
                            int *      aiCode)
     {
     tsNCharcb sOrderSentToExch = {(char *)"order sent to exch",
                                   (int)strlen("order sent to exch")};
     int iIgnored;

     /*   ----------------------------------------------------------------   */

     DEBUG_PRINT("\n\n");
     if (!pInfo -> dump(&iIgnored))
          {
          DEBUG_PRINT("error in pInfo -> dump : %d", iIgnored);
          }

     /*   ----------------------------------------------------------------   */
     /*   record when the order was sent to the exchange... */

     if (pInfo -> sStatus.iDataLen == sOrderSentToExch.iDataLen &&
	 memcmp(pInfo -> sStatus.pData, 
		sOrderSentToExch.pData, 
		sOrderSentToExch.iDataLen) == 0)
	  {
	  g_iToExchSsboe = pInfo -> iSsboe;
	  g_iToExchUsecs = pInfo -> iUsecs;
	  }

     /*   ----------------------------------------------------------------   */
     /*   if there's a completion reason, the order is complete... */

     if (pInfo -> sCompletionReason.pData)
          {
          g_bDone = true;
          }

     /*   ----------------------------------------------------------------   */

     *aiCode = API_OK;
     return(OK);
     }


/*   =====================================================================   */

int MyCallbacks::OpenOrderReplay(OrderReplayInfo * pInfo,
                                 void *            pContext,
                                 int *             aiCode)
     {
     *aiCode = API_OK;
     return(OK);
     }


/*   =====================================================================   */

void write_log(const std::string& level, const std::string& message) {
    fprintf(stderr, "{\"type\":\"log\",\"level\":\"%s\",\"message\":\"%s\"}\n", 
        level.c_str(), message.c_str());
    fflush(stderr);
}

// Helper function to create directory
bool create_directory(const std::string& path) {
    struct stat st = {0};
    if (stat(path.c_str(), &st) == -1) {
        #ifdef _WIN32
            return _mkdir(path.c_str()) == 0;
        #else
            return mkdir(path.c_str(), 0755) == 0;
        #endif
    }
    return true;  // Directory already exists
}

class JsonOrderWriter {
private:
    std::ofstream file;
    bool first_account;
    std::string current_account;
    bool first_order;
    std::string filename;
    bool has_written_account;  // Track if any account has been written
    bool is_closed;  // Track if the JSON has been properly closed
    
public:
    JsonOrderWriter(const std::string& fname) 
        : first_account(true)
        , first_order(true)
        , filename(fname)
        , has_written_account(false)
        , is_closed(false) {
        // Create directory if it doesn't exist
        if (!create_directory("orders")) {
            write_log("error", "Failed to create orders directory");
            return;
        }
        
        file.open(filename);
        if (!file.is_open()) {
            write_log("error", "Failed to open output file: " + filename);
            return;
        }
        
        // Write the initial JSON structure
        file << "{";
        file.flush();
    }
    
    void start_account(const std::string& account_id) {
        if (!file.is_open() || is_closed) return;

        try {
            if (account_id != current_account) {
                if (!first_account && has_written_account) {
                    file << "\n  ]";  // Close previous account's array
                }
                
                if (!first_account) {
                    file << ",";  // Add comma between accounts
                }
                
                file << "\n  \"" << account_id << "\": [";
                
                first_order = true;
                first_account = false;
                current_account = account_id;
                has_written_account = true;
                file.flush();
            }
        } catch (const std::exception& e) {
            write_log("error", "Error in start_account: " + std::string(e.what()));
        }
    }
    
    void write_order(const OrderData& order) {
        if (!file.is_open() || is_closed) return;

        try {
            if (!first_order) {
                file << ",";  // Add comma between orders
            }
            
            file << "\n    {\n";
            file << "      \"order_id\": \"" << order.order_id << "\",\n";
            file << "      \"account_id\": \"" << order.account_id << "\",\n";
            file << "      \"symbol\": \"" << order.symbol << "\",\n";
            file << "      \"exchange\": \"" << order.exchange << "\",\n";
            file << "      \"side\": \"" << order.side << "\",\n";
            file << "      \"order_type\": \"" << order.order_type << "\",\n";
            file << "      \"status\": \"" << order.status << "\",\n";
            file << "      \"quantity\": " << order.quantity << ",\n";
            file << "      \"filled_quantity\": " << order.filled_quantity << ",\n";
            file << "      \"price\": " << order.price << ",\n";
            file << "      \"commission\": " << order.commission << ",\n";
            file << "      \"timestamp\": " << order.timestamp << "\n";
            file << "    }";
            
            first_order = false;
            file.flush();
        } catch (const std::exception& e) {
            write_log("error", "Error in write_order: " + std::string(e.what()));
        }
    }
    
    void finish() {
        if (!file.is_open() || is_closed) return;

        try {
            // Only close the array if we've written at least one account and order
            if (has_written_account) {
                file << "\n  ]";  // Close last account's array
            }
            
            // Always add the metadata fields
            file << ",\n";
            file << "  \"status\": \"complete\",\n";
            file << "  \"timestamp\": \"" << std::time(nullptr) << "\"\n";
            file << "}";
            file.flush();
            file.close();
            is_closed = true;
            write_log("info", "Successfully completed writing orders to " + filename);
        } catch (const std::exception& e) {
            write_log("error", "Error in finish: " + std::string(e.what()));
            // Attempt emergency close if finish fails
            try {
                if (file.is_open() && !is_closed) {
                    if (has_written_account) {
                        file << "\n  ]";  // Close last account's array
                    }
                    file << ",\n";
                    file << "  \"status\": \"error\",\n";
                    file << "  \"timestamp\": \"" << std::time(nullptr) << "\",\n";
                    file << "  \"error\": \"" << escape_json(e.what()) << "\"\n";
                    file << "}";
                    file.flush();
                    file.close();
                    is_closed = true;
                }
            } catch (...) {
                // If all else fails, try one last time to close the JSON
                try {
                    if (file.is_open() && !is_closed) {
                        file << "\n  ],\n";  // Close array and prepare for metadata
                        file << "  \"status\": \"error\",\n";
                        file << "  \"timestamp\": \"" << std::time(nullptr) << "\"\n";
                        file << "}";
                        file.flush();
                        file.close();
                        is_closed = true;
                    }
                } catch (...) {}
            }
        }
    }
    
    void emergency_close() {
        if (!file.is_open() || is_closed) return;

        try {
            if (has_written_account) {
                file << "\n  ]";  // Close last account's array
            }
            file << ",\n";
            file << "  \"status\": \"interrupted\",\n";
            file << "  \"timestamp\": \"" << std::time(nullptr) << "\"\n";
            file << "}";
            file.flush();
            file.close();
            is_closed = true;
            write_log("warning", "Emergency close of order file " + filename);
        } catch (const std::exception& e) {
            write_log("error", "Error in emergency_close: " + std::string(e.what()));
            // Last resort attempt to close JSON properly
            try {
                if (file.is_open() && !is_closed) {
                    file << "\n  ],\n";  // Close array and prepare for metadata
                    file << "  \"status\": \"interrupted\",\n";
                    file << "  \"timestamp\": \"" << std::time(nullptr) << "\"\n";
                    file << "}";
                    file.flush();
                    file.close();
                    is_closed = true;
                }
            } catch (...) {}
        }
    }
    
    ~JsonOrderWriter() {
        if (file.is_open() && !is_closed) {
            emergency_close();
        }
    }
};

int MyCallbacks::OrderReplay(OrderReplayInfo* pInfo, void* pContext, int* aiCode) {
    if (!pInfo || !aiCode) {
        fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Invalid parameters in OrderReplay callback\"}\n");
        return BAD;
    }

    std::lock_guard<std::mutex> lock(g_writer_mutex);
    
    if (pInfo->iArrayLen > 0 && pInfo->asLineInfoArray) {
        // Create writer if it doesn't exist
        if (!g_writer) {
            g_orders_filename = "orders/orders_" + std::to_string(time(nullptr)) + ".json";
            try {
                g_writer = new JsonOrderWriter(g_orders_filename);
                if (!g_writer) {
                    throw std::runtime_error("Failed to allocate JsonOrderWriter");
                }
                write_log("info", "Created order output file: " + g_orders_filename);
            } catch (const std::exception& e) {
                write_log("error", "Failed to create JsonOrderWriter: " + std::string(e.what()));
                *aiCode = API_OK;
                return(OK);
            }
        }
        
        for (int i = 0; i < pInfo->iArrayLen; i++) {
            if (i >= 10000) { // Safety limit for array size
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"warning\",\"message\":\"Reached maximum order limit of 10000\"}\n");
                break;
            }

            const auto& lineInfo = pInfo->asLineInfoArray[i];
            
            // Validate line info data
            if (!isValidString(&lineInfo.sOrderNum) ||
                !isValidString(&lineInfo.sTicker) ||
                !isValidString(&lineInfo.sExchange) ||
                !isValidString(&lineInfo.sBuySellType) ||
                !isValidString(&lineInfo.sOrderType) ||
                !isValidString(&lineInfo.sStatus) ||
                !isValidAccount(&lineInfo.oAccount)) {
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Invalid line info data at index %d\"}\n", i);
                continue;
            }

            // Skip orders with no fills or invalid data
            if (lineInfo.llFilled <= 0) {
                continue;
            }
            
            try {
                OrderData orderData;
                
                // Basic order info with safety checks and size limits
                auto copyString = [](const tsNCharcb& src) -> std::string {
                    if (!src.pData || src.iDataLen <= 0 || src.iDataLen >= 1024) {
                        throw std::runtime_error("Invalid string data");
                    }
                    return std::string(src.pData, src.iDataLen);
                };

                orderData.order_id = copyString(lineInfo.sOrderNum);
                orderData.account_id = copyString(lineInfo.oAccount.sAccountId);
                orderData.symbol = copyString(lineInfo.sTicker);
                orderData.exchange = copyString(lineInfo.sExchange);
                orderData.side = copyString(lineInfo.sBuySellType);
                orderData.order_type = copyString(lineInfo.sOrderType);
                orderData.status = copyString(lineInfo.sStatus);
                
                // Quantity information with bounds checking
                if (lineInfo.llQuantityToFill < 0 || lineInfo.llQuantityToFill > 1000000000) {
                    throw std::runtime_error("Invalid quantity");
                }
                orderData.quantity = lineInfo.llQuantityToFill;
                orderData.filled_quantity = lineInfo.llFilled;
                
                // Price validation
                if (std::isnan(lineInfo.dPriceToFill) || std::isinf(lineInfo.dPriceToFill) ||
                    std::isnan(lineInfo.dAvgFillPrice) || std::isinf(lineInfo.dAvgFillPrice)) {
                    throw std::runtime_error("Invalid price data");
                }

                // use average fill price
                orderData.price = lineInfo.dAvgFillPrice;
                
                // Get commission using the new structure with bounds checking
                std::string symbol = orderData.symbol;
                std::string product_code = symbol.length() > 2 ? 
                    symbol.substr(0, symbol.length() - 2) : symbol;
                
                auto commission_it = g_commission_rates.find(product_code);
                if (commission_it != g_commission_rates.end() && commission_it->second.is_valid) {
                    double commission_rate = commission_it->second.rate;
                    if (commission_rate < 0 || commission_rate > 100) {
                        throw std::runtime_error("Invalid commission rate");
                    }
                    orderData.commission = lineInfo.llFilled * commission_rate;
                } else {
                    orderData.commission = 0.0;
                }
                
                // Timestamp validation
                if (lineInfo.iSsboe < 0) {
                    throw std::runtime_error("Invalid timestamp");
                }
                orderData.timestamp = lineInfo.iSsboe;

                // Write order to file with error handling
                if (g_writer) {
                    g_writer->start_account(orderData.account_id);
                    g_writer->write_order(orderData);
                }
                
                // Update account stats with bounds checking
                auto& account_orders = g_AccountOrdersMap[orderData.account_id];
                if (account_orders.size() < 1000000) { // Reasonable limit for orders per account
                    account_orders.push_back(orderData);
                    
                    auto& stats = g_AccountStats[orderData.account_id];
                    stats.orders_processed++;

                    // Send progress update every 100 orders
                    if (stats.orders_processed % 100 == 0) {
                        std::stringstream progress;
                        progress << "Account " << orderData.account_id 
                                << ": processed " << stats.orders_processed 
                                << " orders (" << stats.days_processed 
                                << "/" << stats.total_days << " days)";
                        write_log("info", progress.str());
                    }
                } else {
                    throw std::runtime_error("Exceeded maximum orders per account");
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Failed to process order: %s\"}\n",
                    e.what());
                continue;
            }
        }
    }

    // Signal completion and update days processed
    g_bRcvdOrderReplay = true;
    if (g_OutstandingRequests > 0) {
        g_OutstandingRequests--;
        
        // Update days processed for the current account with bounds checking
        if (!g_AccountList.empty() && g_CurrentAccountIndex < g_AccountList.size()) {
            const auto& current_account = g_AccountList[g_CurrentAccountIndex];
            if (isValidAccount(&current_account)) {
                std::string current_account_id(current_account.sAccountId.pData,
                                            current_account.sAccountId.iDataLen);
                auto& stats = g_AccountStats[current_account_id];
                if (stats.days_processed < stats.total_days) {  // Add bounds check
                    stats.days_processed++;
                }
                
                std::stringstream progress;
                progress << "Account " << current_account_id 
                        << ": processed " << stats.orders_processed 
                        << " orders (" << stats.days_processed 
                        << "/" << stats.total_days << " days)";
                write_log("info", progress.str());
            }
        }
    }

    write_log("info", "Completed order replay batch. Outstanding requests: " + std::to_string(g_OutstandingRequests));

    *aiCode = API_OK;
    return(OK);
}


/*   =====================================================================   */

int MyCallbacks::PnlReplay(PnlReplayInfo * pInfo,
                           void *          pContext,
                           int *           aiCode)
     {
     *aiCode = API_OK;
     return(OK);
     }

/*   =====================================================================   */

int MyCallbacks::PnlUpdate(PnlInfo * pInfo,
                           void *    pContext,
                           int *     aiCode)
     {
     *aiCode = API_OK;
     return(OK);
     }

/*   =====================================================================   */

int MyCallbacks::SingleOrderReplay(SingleOrderReplayInfo * pInfo,
				   void *                  pContext,
				   int *                   aiCode)
     {
     if (pInfo->iRpCode == 0) {  // Success
        // Process additional order details here if needed
        // This callback will receive more detailed information about the order
        
        DEBUG_PRINT("Received additional details for order: %.*s", 
            pInfo->sOrderNum.iDataLen, pInfo->sOrderNum.pData);
    }
    
    *aiCode = API_OK;
    return(OK);
     }

/*   =====================================================================   */

// Modify isDateGreaterOrEqual function
bool isDateGreaterOrEqual(const std::string& date1, const std::string& date2) {
    // Validate input lengths
    if (date1.length() != 8 || date2.length() != 8) {
        return false;
    }

    try {
        // Extract year, month, and day from date1
        int year1 = std::stoi(date1.substr(0, 4));
        int month1 = std::stoi(date1.substr(4, 2));
        int day1 = std::stoi(date1.substr(6, 2));

        // Extract year, month, and day from date2
        int year2 = std::stoi(date2.substr(0, 4));
        int month2 = std::stoi(date2.substr(4, 2));
        int day2 = std::stoi(date2.substr(6, 2));

        // Compare dates
        if (year1 > year2) return true;
        if (year1 < year2) return false;
        if (month1 > month2) return true;
        if (month1 < month2) return false;
        return day1 >= day2;
    } catch (const std::exception& e) {
        fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Error comparing dates: %s\"}\n", e.what());
        return false;
    }
}

// Modify the date filtering section in OrderHistoryDates
int MyCallbacks::OrderHistoryDates(OrderHistoryDatesInfo* pInfo, void* pContext, int* aiCode) {
    if (!pInfo || !aiCode) {
        fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Invalid parameters in OrderHistoryDates callback\"}\n");
        return BAD;
    }

    if (pInfo->iArrayLen > 0 && pInfo->asDateArray) {
        try {
            // Create a vector of date strings for sorting with validation
            std::vector<std::pair<std::string, tsNCharcb> > dates;
            dates.reserve(pInfo->iArrayLen); // Pre-allocate space

            // Initialize account stats with total days
            if (!g_AccountList.empty() && g_CurrentAccountIndex < g_AccountList.size()) {
                const auto& current_account = g_AccountList[g_CurrentAccountIndex];
                if (isValidAccount(&current_account)) {
                    std::string current_account_id(current_account.sAccountId.pData,
                                                current_account.sAccountId.iDataLen);
                    g_AccountStats[current_account_id] = ProcessingStats(pInfo->iArrayLen, 0, 0);
                }
            }
            
            // Process all dates
            for (int i = 0; i < pInfo->iArrayLen; i++) {
                if (!isValidString(&pInfo->asDateArray[i])) {
                    fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Invalid date string at index %d\"}\n", i);
                    continue;
                }
                
                std::string dateStr(pInfo->asDateArray[i].pData, 
                                  pInfo->asDateArray[i].iDataLen);
                
                // Validate date format (YYYYMMDD)
                if (dateStr.length() != 8 || !std::all_of(dateStr.begin(), dateStr.end(), ::isdigit)) {
                    fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Invalid date format: %s\"}\n", 
                        dateStr.c_str());
                    continue;
                }

                if (dateStr.length() == 8 && isDateGreaterOrEqual(dateStr, g_start_date)) {
                    dates.push_back(std::make_pair(dateStr, pInfo->asDateArray[i]));
                }
            }

            if (dates.empty()) {
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"No valid dates found\"}\n");
                g_bRcvdHistoryDates = true;
                *aiCode = API_OK;
                return OK;
            }

            // Sort dates in descending order (most recent first)
            struct DateCompare {
                bool operator()(const std::pair<std::string, tsNCharcb>& a,
                              const std::pair<std::string, tsNCharcb>& b) {
                    return a.first > b.first;
                }
            };

            std::sort(dates.begin(), dates.end(), DateCompare());

            // Clean up any existing history dates
            if (g_HistoryDates) {
                for (int i = 0; i < g_iNumDatesReceived; i++) {
                    safeDeleteString(g_HistoryDates[i]);
                }
                delete[] g_HistoryDates;
                g_HistoryDates = nullptr;
            }

            // Use max_days from global variable
            g_iNumDatesReceived = std::min(g_max_days, static_cast<int>(dates.size()));
            
            try {
                g_HistoryDates = new tsNCharcb[g_iNumDatesReceived];
                memset(g_HistoryDates, 0, sizeof(tsNCharcb) * g_iNumDatesReceived);
                
                for (int i = 0; i < g_iNumDatesReceived; i++) {
                    if (dates[i].second.iDataLen > 0 && dates[i].second.iDataLen < 1024) {
                        g_HistoryDates[i].iDataLen = dates[i].second.iDataLen;
                        g_HistoryDates[i].pData = new char[g_HistoryDates[i].iDataLen + 1];
                        memcpy(g_HistoryDates[i].pData, 
                               dates[i].second.pData, 
                               g_HistoryDates[i].iDataLen);
                        g_HistoryDates[i].pData[g_HistoryDates[i].iDataLen] = '\0';
                    } else {
                        fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Invalid date length at index %d\"}\n", i);
                        g_HistoryDates[i].pData = nullptr;
                        g_HistoryDates[i].iDataLen = 0;
                    }
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Failed to allocate history dates: %s\"}\n",
                    e.what());
                    
                // Clean up on error
                if (g_HistoryDates) {
                    for (int i = 0; i < g_iNumDatesReceived; i++) {
                        safeDeleteString(g_HistoryDates[i]);
                    }
                    delete[] g_HistoryDates;
                    g_HistoryDates = nullptr;
                }
                g_iNumDatesReceived = 0;
                *aiCode = API_CQ_ERROR;
                return BAD;
            }

            g_bRcvdHistoryDates = true;
            *aiCode = API_OK;
            return OK;

        } catch (const std::exception& e) {
            fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Exception in OrderHistoryDates: %s\"}\n",
                e.what());
            *aiCode = API_CQ_ERROR;
            return BAD;
        }
    }

    g_bRcvdHistoryDates = true;
    *aiCode = API_OK;
    return OK;
}

/*   =====================================================================   */

int MyCallbacks::ProductRmsList(ProductRmsListInfo* pInfo, void* pContext, int* aiCode) {
    if (pInfo && pInfo->iArrayLen > 0) {
        
        for (int i = 0; i < pInfo->iArrayLen; i++) {
            const auto& rmsInfo = pInfo->asProductRmsInfoArray[i];
            
            std::string product_code(rmsInfo.sProductCode.pData, rmsInfo.sProductCode.iDataLen);
            
            CommissionRate commission = {
                .rate = rmsInfo.bCommissionFillRate ? rmsInfo.dCommissionFillRate : 0.0,
                .is_valid = rmsInfo.bCommissionFillRate
            };
            
            g_commission_rates[product_code] = commission;
        }
    }
    *aiCode = API_OK;
    return(OK);
}

/*   =====================================================================   */

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

bool sendOrdersToAPI(const std::vector<OrderData>& orders) {
    // Create JSON output
    std::stringstream json;
    
    // Group orders by account_id
    std::map<std::string, std::vector<OrderData>> ordersByAccount;
    for (const auto& order : orders) {
        if (order.filled_quantity > 0) {
            ordersByAccount[order.account_id].push_back(order);
        }
    }
    
    json << "{\n";
    
    // Add orders grouped by account
    bool firstAccount = true;
    for (const auto& accountPair : ordersByAccount) {
        if (!firstAccount) {
            json << ",\n";
        }
        firstAccount = false;
        
        json << "  \"" << accountPair.first << "\": [\n";
        
        const auto& accountOrders = accountPair.second;
        for (size_t i = 0; i < accountOrders.size(); ++i) {
            const auto& order = accountOrders[i];
            json << "    {\n"
                 << "      \"order_id\": \"" << order.order_id << "\",\n"
                 << "      \"account_id\": \"" << order.account_id << "\",\n"
                 << "      \"symbol\": \"" << order.symbol << "\",\n"
                 << "      \"exchange\": \"" << order.exchange << "\",\n"
                 << "      \"side\": \"" << order.side << "\",\n"
                 << "      \"order_type\": \"" << order.order_type << "\",\n"
                 << "      \"status\": \"" << order.status << "\",\n"
                 << "      \"quantity\": " << order.quantity << ",\n"
                 << "      \"filled_quantity\": " << order.filled_quantity << ",\n"
                 << "      \"price\": " << order.price << ",\n"
                 << "      \"commission\": " << order.commission << ",\n"
                 << "      \"timestamp\": " << order.timestamp << "\n"
                 << "    }" << (i < accountOrders.size() - 1 ? "," : "") << "\n";
        }
        json << "  ]";
    }
    
    // Add metadata at the end
    json << ",\n"
         << "  \"status\": \"complete\",\n"
         << "  \"timestamp\": \"" << std::time(nullptr) << "\"\n"
         << "}";

    // Write JSON to stdout
    fprintf(stdout, "%s\n", json.str().c_str());
    fflush(stdout); // Ensure output is written immediately
    return true;
}

void broadcastUpdate(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int client_fd : g_websocket_clients) {
        // Format message according to WebSocket protocol
        std::cout << "Broadcasting to client: " << message << std::endl;
        // You can also send this to your websocket or logging system
    }
}

// Add date validation function after other helper functions
bool isValidDateFormat(const std::string& date) {
    if (date.length() != 8) return false;
    if (!std::all_of(date.begin(), date.end(), ::isdigit)) return false;
    
    int year = std::stoi(date.substr(0, 4));
    int month = std::stoi(date.substr(4, 2));
    int day = std::stoi(date.substr(6, 2));
    
    if (year < 1900 || year > 2100) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;
    
    return true;
}

// Server configuration structure
struct ServerConfig {
    std::string dmn_srvr_addr;
    std::string domain_name;
    std::string lic_srvr_addr;
    std::string loc_brok_addr;
    std::string logger_addr;
    std::string log_type;
    std::string ssl_clnt_auth_file;
    std::string user;
};

// Function to load server configuration
ServerConfig load_server_config(const std::string& server_type, const std::string& location) {
    std::ifstream f("server_configurations.json");
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open server_configurations.json");
    }

    json config;
    f >> config;

    if (!config.contains(server_type)) {
        throw std::runtime_error("Invalid server type: " + server_type);
    }

    const auto& server_config = config[server_type];
    if (!server_config["server_configs"].contains(location)) {
        throw std::runtime_error("Invalid location for server type " + server_type + ": " + location);
    }

    const auto& loc_config = server_config["server_configs"][location];
    
    ServerConfig cfg;
    cfg.dmn_srvr_addr = loc_config["MML_DMN_SRVR_ADDR"];
    cfg.domain_name = loc_config["MML_DOMAIN_NAME"];
    cfg.lic_srvr_addr = loc_config["MML_LIC_SRVR_ADDR"];
    cfg.loc_brok_addr = loc_config["MML_LOC_BROK_ADDR"];
    cfg.logger_addr = loc_config["MML_LOGGER_ADDR"];
    cfg.log_type = loc_config["MML_LOG_TYPE"];
    cfg.ssl_clnt_auth_file = loc_config["MML_SSL_CLNT_AUTH_FILE"];
    cfg.user = loc_config["USER"];

    return cfg;
}

int main(int argc, char** argv, char** envp)
{
    char* USAGE = (char*)"OrderFetcher user password server_type location start_date [account_ids...]\n";
    
    MyAdmCallbacks *  pAdmCallbacks;
    RCallbacks *      pCallbacks;
    REngineParams     oParams;
    LoginParams       oLoginParams;
    char *            fake_envp[9];
    int               iCode;

    /*   ----------------------------------------------------------------   */

    if (argc < 6)
    {
        DEBUG_PRINT("%s", USAGE);
        return (BAD);
    }

    // Store start date
    g_start_date = argv[5];

    // Validate date format
    if (!isValidDateFormat(g_start_date)) {
        ERROR_PRINT("Invalid date format. Please use YYYYMMDD format.");
        return (BAD);
    }

    try {
        // Load server configurations based on server type and location
        std::ifstream config_file("server_configurations.json");
        if (!config_file.is_open()) {
            ERROR_PRINT("Failed to open server_configurations.json\n");
            return BAD;
        }

        json server_configs;
        config_file >> server_configs;
        config_file.close();

        // Find the server configuration for the given server type and location
        if (!server_configs.contains(argv[3])) {
            ERROR_PRINT("Server type %s not found in configurations\n", argv[3]);
            return BAD;
        }

        const auto& server_json = server_configs[argv[3]]["server_configs"][argv[4]];
        if (server_json.is_null()) {
            ERROR_PRINT("Location %s not found for server type %s\n", argv[4], argv[3]);
            return BAD;
        }

        // Log connection attempt
        DEBUG_PRINT("Connecting to server...");

        // Set environment variables from configuration
        fake_envp[0] = strdup(("MML_DMN_SRVR_ADDR=" + server_json["MML_DMN_SRVR_ADDR"].get<std::string>()).c_str());
        fake_envp[1] = strdup(("MML_DOMAIN_NAME=" + server_json["MML_DOMAIN_NAME"].get<std::string>()).c_str());
        fake_envp[2] = strdup(("MML_LIC_SRVR_ADDR=" + server_json["MML_LIC_SRVR_ADDR"].get<std::string>()).c_str());
        fake_envp[3] = strdup(("MML_LOC_BROK_ADDR=" + server_json["MML_LOC_BROK_ADDR"].get<std::string>()).c_str());
        fake_envp[4] = strdup(("MML_LOGGER_ADDR=" + server_json["MML_LOGGER_ADDR"].get<std::string>()).c_str());
        fake_envp[5] = strdup("MML_LOG_TYPE=log_net");
        fake_envp[6] = strdup("MML_SSL_CLNT_AUTH_FILE=rithmic_ssl_cert_auth_params");
        fake_envp[7] = strdup("USER=your_user_name");
        fake_envp[8] = NULL;

        try {
            pAdmCallbacks = new MyAdmCallbacks();
            if (!pAdmCallbacks) {
                ERROR_PRINT("Failed to allocate MyAdmCallbacks");
                return BAD;
            }
        } catch (OmneException& oEx) {
            iCode = oEx.getErrorCode();
            ERROR_PRINT("MyAdmCallbacks::MyAdmCallbacks() error : %d", iCode);
            return BAD;
        }

        /*   ----------------------------------------------------------------   */

        oParams.sAppName.pData        = "DeltalytixRithmicAPI";
        oParams.sAppName.iDataLen     = (int)strlen(oParams.sAppName.pData);
        oParams.sAppVersion.pData     = "1.0.0.0";
        oParams.sAppVersion.iDataLen  = (int)strlen(oParams.sAppVersion.pData);
        oParams.envp                  = fake_envp;
        oParams.pAdmCallbacks         = pAdmCallbacks;
        oParams.sLogFilePath.pData    = "so.log";
        oParams.sLogFilePath.iDataLen = (int)strlen(oParams.sLogFilePath.pData);

        try {
            g_pEngine = new REngine(&oParams);
            if (!g_pEngine) {
                ERROR_PRINT("Failed to allocate REngine");
                delete pAdmCallbacks;
                return BAD;
            }
        } catch (OmneException& oEx) {
            delete pAdmCallbacks;
            iCode = oEx.getErrorCode();
            ERROR_PRINT("REngine::REngine() error : %d", iCode);
            return BAD;
        }

        try {
            pCallbacks = new MyCallbacks();
            if (!pCallbacks) {
                ERROR_PRINT("Failed to allocate MyCallbacks");
                delete g_pEngine;
                delete pAdmCallbacks;
                return BAD;
            }
        } catch (OmneException& oEx) {
            delete g_pEngine;
            delete pAdmCallbacks;
            iCode = oEx.getErrorCode();
            ERROR_PRINT("MyCallbacks::MyCallbacks() error : %d", iCode);
            return BAD;
        }

        /*   ----------------------------------------------------------------   */
        /*   Set up parameters for logging in.  */

        oLoginParams.pCallbacks = pCallbacks;

        // Remove market data connection parameters since we don't need them
        oLoginParams.sMdUser.pData = NULL;
        oLoginParams.sMdUser.iDataLen = 0;
        oLoginParams.sMdPassword.pData = NULL;
        oLoginParams.sMdPassword.iDataLen = 0;
        oLoginParams.sMdCnnctPt.pData = NULL;
        oLoginParams.sMdCnnctPt.iDataLen = 0;

        // Get trading system connection point from configuration
        const std::string ts_point = server_json["TS_CNNCT_PT"].get<std::string>();

        if (ts_point.empty()) {
            ERROR_PRINT("Missing trading system connection point in configuration\n");
            delete g_pEngine;
            delete pCallbacks;
            delete pAdmCallbacks;
            return BAD;
        }

        oLoginParams.sTsCnnctPt.pData = strdup(ts_point.c_str());
        oLoginParams.sTsCnnctPt.iDataLen = (int)strlen(ts_point.c_str());

        oLoginParams.sTsUser.pData = argv[1];
        oLoginParams.sTsUser.iDataLen = (int)strlen(oLoginParams.sTsUser.pData);

        oLoginParams.sTsPassword.pData = argv[2];
        oLoginParams.sTsPassword.iDataLen = (int)strlen(oLoginParams.sTsPassword.pData);

        /*   ----------------------------------------------------------------   */

        if (!g_pEngine->login(&oLoginParams, &iCode))
        {
            ERROR_PRINT("REngine::login() error : %d\n", iCode);

            delete g_pEngine;
            delete pCallbacks;
            delete pAdmCallbacks;

            // Clean up environment variables
            for (int i = 0; i < 8; i++) {
                delete[] fake_envp[i];
            }

            // Clean up connection point strings
            free((void*)oLoginParams.sTsCnnctPt.pData);

            return BAD;
        }

        /*   ----------------------------------------------------------------   */
        /*   After calling REngine::login, RCallbacks::Alert will be called a   */
        /*   number of times.  Wait for when the login to the TsCnnctPt is      */
        /*   complete.  (See MyCallbacks::Alert() for details).                 */

        while (!g_bTsLoginComplete)
        {
            sleep(1);
        }

        // Wait for account info to be received
        fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Waiting for account info...\"}\n");
        fflush(stderr);

        while (!g_bRcvdAccount)
        {
            sleep(1);
        }

        // Use safe print function for account info
        if (g_oAccount.sAccountId.pData && g_oAccount.sAccountId.iDataLen > 0) {
            std::string account_id(g_oAccount.sAccountId.pData, g_oAccount.sAccountId.iDataLen);
            fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Account info received for account: %s\"}\n", 
                account_id.c_str());
            fflush(stderr);
        }

        // After login succeeds, process any requested account IDs
        if (argc > 6) {
            for (int i = 6; i < argc; i++) {
                g_RequestedAccounts.insert(argv[i]);
            }
            fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Will process %zu specific accounts\"}\n",
                g_RequestedAccounts.size());
            fflush(stderr);
        }

        // Instead, process each account fully
        while (g_CurrentAccountIndex < g_AccountList.size())
        {
            AccountInfo& currentAccount = g_AccountList[g_CurrentAccountIndex];
            std::string current_account_id(currentAccount.sAccountId.pData, currentAccount.sAccountId.iDataLen);

            // Skip accounts not in the requested list if we have specific requests
            if (!g_RequestedAccounts.empty() && 
                g_RequestedAccounts.find(current_account_id) == g_RequestedAccounts.end()) {
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Skipping account %s (not in requested list)\"}\n",
                    current_account_id.c_str());
                fflush(stderr);
                g_CurrentAccountIndex++;
                continue;
            }

            fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Starting processing for account %zu of %zu: %s\"}\n",
                g_CurrentAccountIndex + 1,
                g_AccountList.size(),
                current_account_id.c_str());
            fflush(stderr);

            // Clear previous account's state
            g_commission_rates.clear();
            
            // Unsubscribe from previous account's orders if not the first account
            if (g_CurrentAccountIndex > 0) {
                AccountInfo& prevAccount = g_AccountList[g_CurrentAccountIndex - 1];
                if (!g_pEngine->unsubscribeOrder(&prevAccount, &iCode)) {
                    fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Failed to unsubscribe from previous account's orders\"}\n");
                    fflush(stderr);
                }
                // Add small delay after unsubscribe
                usleep(100000);  // 100ms
            }

            // Verify account data integrity
            if (!currentAccount.sAccountId.pData || currentAccount.sAccountId.iDataLen <= 0) {
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Invalid account data detected\"}\n");
                fflush(stderr);
                g_CurrentAccountIndex++;
                continue;
            }

            // Request RMS info for current account
            fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Requesting RMS info for account: %s\"}\n", 
                current_account_id.c_str());
            fflush(stderr);

            // Add delay before RMS request
            usleep(100000);  // 100ms

            int rmsRetryCount = 3;
            bool rmsSuccess = false;
            while (rmsRetryCount > 0 && !rmsSuccess) {
                if (!g_pEngine->getProductRmsInfo(&currentAccount, &iCode)) {
                    fprintf(stderr, "{\"type\":\"log\",\"level\":\"warning\",\"message\":\"REngine::getProductRmsInfo() retry %d error: %d\"}\n", 
                       4 - rmsRetryCount, iCode);
                    fflush(stderr);
                    rmsRetryCount--;
                    if (rmsRetryCount > 0) {
                        usleep(500000);  // 500ms delay between retries
                    }
                } else {
                    rmsSuccess = true;
                    fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Successfully requested Product RMS info\"}\n");
                    fflush(stderr);
                }
            }

            if (!rmsSuccess) {
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Failed to get RMS info after all retries for account %s\"}\n",
                    current_account_id.c_str());
                fflush(stderr);
                g_CurrentAccountIndex++;
                continue;
            }

            // Add delay after RMS request
            usleep(100000);  // 100ms

            // Create a vector for this account's orders if it doesn't exist
            if (g_AccountOrdersMap.find(current_account_id) == g_AccountOrdersMap.end()) {
                g_AccountOrdersMap[current_account_id] = std::vector<OrderData>();
            }
            
            DEBUG_PRINT("\nProcessing account %zu of %zu: %.*s\n", 
                g_CurrentAccountIndex + 1,
                g_AccountList.size(),
                currentAccount.sAccountId.iDataLen, 
                currentAccount.sAccountId.pData);

            // Reset flags for each account
            g_bRcvdHistoryDates = false;
            g_bRcvdOrderReplay = false;
            g_iNumOrdersReceived = 0;
            g_OutstandingRequests = 0;
            
            // Clear previous history dates if any
            if (g_HistoryDates != NULL) {
                for (int i = 0; i < g_iNumDatesReceived; i++) {
                    if (g_HistoryDates[i].pData != NULL) {
                        delete[] g_HistoryDates[i].pData;
                        g_HistoryDates[i].pData = NULL;  // Set to NULL after delete
                    }
                }
                delete[] g_HistoryDates;
                g_HistoryDates = NULL;
            }
            g_iNumDatesReceived = 0;
            g_DateProcessed.clear();

            // Get history dates first to know total number of dates
            void* pContext = NULL;
            fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Requesting history dates for account: %s\"}\n", 
                current_account_id.c_str());
            fflush(stderr);

            if (!g_pEngine->listOrderHistoryDates(pContext, &iCode))
            {
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Failed to get history dates for account %s: error %d\"}\n",
                    current_account_id.c_str(), iCode);
                fflush(stderr);
                g_CurrentAccountIndex++;
                continue;
            }

            // Wait for dates with timeout
            int timeout = 30; // 30 seconds timeout
            while (!g_bRcvdHistoryDates && timeout > 0)
            {
                sleep(1);
                timeout--;
                if (timeout % 5 == 0) { // Log every 5 seconds
                    fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Waiting for history dates... %d seconds remaining\"}\n", 
                        timeout);
                    fflush(stderr);
                }
            }

            if (!g_bRcvdHistoryDates) {
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Timeout waiting for history dates for account %s\"}\n",
                    current_account_id.c_str());
                fflush(stderr);
                g_CurrentAccountIndex++;
                continue;
            }

            // Calculate total number of dates (including current session)
            int total_dates = g_iNumDatesReceived + 1;  // +1 for current session

            // Add delay before subscribe
            usleep(100000);  // 100ms

            // Subscribe to the current account's orders
            fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Subscribing to orders for account: %s\"}\n", 
                current_account_id.c_str());
            fflush(stderr);

            int subscribeRetryCount = 3;
            bool subscribeSuccess = false;
            while (subscribeRetryCount > 0 && !subscribeSuccess) {
                if (!g_pEngine->subscribeOrder(&currentAccount, &iCode)) {
                    fprintf(stderr, "{\"type\":\"log\",\"level\":\"warning\",\"message\":\"Failed to subscribe to orders for account %s: error %d (retry %d)\"}\n",
                        current_account_id.c_str(), iCode, 4 - subscribeRetryCount);
                    fflush(stderr);
                    subscribeRetryCount--;
                    if (subscribeRetryCount > 0) {
                        usleep(500000);  // 500ms delay between retries
                    }
                } else {
                    subscribeSuccess = true;
                    fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Successfully subscribed to orders for account: %s\"}\n", 
                        current_account_id.c_str());
                    fflush(stderr);
                }
            }

            if (!subscribeSuccess) {
                fprintf(stderr, "{\"type\":\"log\",\"level\":\"error\",\"message\":\"Failed to subscribe after all retries for account %s\"}\n",
                    current_account_id.c_str());
                fflush(stderr);
                g_CurrentAccountIndex++;
                continue;
            }

            // Add delay after subscribe
            usleep(100000);  // 100ms

            // Process current session orders as date 1 of total_dates
            g_bRcvdOrderReplay = false;
            
            // Get current date in YYYYMMDD format
            time_t now = time(nullptr);
            struct tm* timeinfo = localtime(&now);
            char current_date[9];
            strftime(current_date, sizeof(current_date), "%Y%m%d", timeinfo);

            fprintf(stderr, "{\"type\":\"progress\",\"message\":\"[%s] Processing date 1/%d: %s\"}\n", 
                current_account_id.c_str(), total_dates, current_date);
            fflush(stderr);

            if (!g_pEngine->replayAllOrders(&currentAccount, 0, 0, &iCode)) {
                fprintf(stderr, "{\"type\":\"error\",\"message\":\"Failed to get orders for date %s\"}\n",
                    current_date);
                fflush(stderr);
            } else {
                // Wait for current session orders with shorter timeout and faster polling
                timeout = 10; // Reduced from 30 to 10 seconds
                while (!g_bRcvdOrderReplay && timeout > 0) {
                    usleep(50000); // Check every 50ms instead of 1s
                    timeout--;
                }
            }

            // Add minimal delay between dates
            usleep(5000);  // 5ms delay

            // Now process historical dates one at a time
            for (int i = 0; i < g_iNumDatesReceived; i++) {
                // Verify date data integrity
                if (!g_HistoryDates[i].pData || g_HistoryDates[i].iDataLen <= 0) {
                    continue;
                }

                // Create a local copy of the date for safety
                tsNCharcb dateCopy;
                dateCopy.iDataLen = g_HistoryDates[i].iDataLen;
                dateCopy.pData = new char[dateCopy.iDataLen + 1];
                memcpy(dateCopy.pData, g_HistoryDates[i].pData, dateCopy.iDataLen);
                dateCopy.pData[dateCopy.iDataLen] = '\0';  // Null terminate for safety

                fprintf(stderr, "{\"type\":\"progress\",\"message\":\"[%s] Processing date %d/%d: %s\"}\n",
                    current_account_id.c_str(), i + 2, total_dates, dateCopy.pData);
                fflush(stderr);

                // Reset flag before request
                g_bRcvdOrderReplay = false;

                // Request historical orders
                if (!g_pEngine->replayHistoricalOrders(&currentAccount, &dateCopy, &iCode)) {
                    fprintf(stderr, "{\"type\":\"error\",\"message\":\"Failed to get orders for date %s\"}\n",
                        dateCopy.pData);
                    delete[] dateCopy.pData;
                    continue;
                }

                // Wait for response with shorter timeout and faster polling
                timeout = 10;  // Reduced from 30 to 10 seconds
                while (!g_bRcvdOrderReplay && timeout > 0) {
                    usleep(50000); // Check every 50ms instead of 1s
                    timeout--;
                }

                delete[] dateCopy.pData;

                // Add minimal delay between dates
                usleep(5000);  // 5ms delay
            }

            // Send completion message for this account
            std::stringstream completion_json;
            completion_json << "{"
                << "\"type\":\"account_complete\","
                << "\"message\":\"Completed account " << current_account_id << " ("
                << g_CurrentAccountIndex + 1 << "/" << g_AccountList.size() << "): "
                << g_AccountOrdersMap[current_account_id].size() << " orders processed\""
                << "}";
            fprintf(stderr, "%s\n", completion_json.str().c_str());
            fflush(stderr);

            g_CurrentAccountIndex++;
        }

        // After all accounts are processed, combine all orders
        g_OrderDataList.clear();
        for (const auto& account_orders : g_AccountOrdersMap) {
            g_OrderDataList.insert(g_OrderDataList.end(), 
                account_orders.second.begin(), 
                account_orders.second.end());
        }

        // Ensure we properly close the JSON writer
        {
            std::lock_guard<std::mutex> lock(g_writer_mutex);
            if (g_writer) {
                g_writer->finish();
                delete g_writer;
                g_writer = nullptr;
            }
        }

        // Send final completion message
        std::stringstream final_json;
        final_json << "{"
            << "\"type\":\"complete\","
            << "\"status\":\"all_complete\","
            << "\"total_accounts_available\":" << g_AccountList.size() << ","
            << "\"accounts_processed\":" << (g_RequestedAccounts.empty() ? g_AccountList.size() : g_RequestedAccounts.size()) << ","
            << "\"total_orders\":" << g_OrderDataList.size() << ","
            << "\"orders_file\":\"" << g_orders_filename << "\""
            << "}";
        fprintf(stdout, "%s\n", final_json.str().c_str());
        fflush(stdout);

        // Use the centralized cleanup function
        cleanupResources();

        // Close log file if open
        if (g_log_file.is_open()) {
            log_to_file("INFO", "OrderFetcher completed");
            g_log_file.close();
        }

        // Clean up dynamically allocated environment variables
        for (int i = 0; i < 8; i++) {
            delete[] fake_envp[i];
        }

        // Clean up connection point strings
        free((void*)oLoginParams.sTsCnnctPt.pData);

        return GOOD;

    } catch (const std::exception& e) {
        ERROR_PRINT("Error: %s", e.what());
        return BAD;
    }
}


