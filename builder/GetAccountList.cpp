#include "RApiPlus.h"
#include "json.hpp"

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

using json = nlohmann::json;

// Add these constants and definitions near the top after existing includes
#define GOOD 0
#define BAD  1

// Simplified struct for account data
struct AccountData {
    std::string account_id;
    std::string fcm_id;
    std::string ib_id;
};

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

// Global variables - remove unnecessary ones and keep only what's needed
using namespace RApi;
bool g_bTsLoginComplete = false;
bool g_bRcvdAccount = false;
std::vector<AccountData> g_AccountList;
int g_iToExchSsboe = 0;
int g_iToExchUsecs = 0;
int g_iFromExchSsboe = 0;
int g_iFromExchUsecs = 0;
REngine* g_pEngine = nullptr;

const int g_iMAX_LEN = 256;
char g_cAccountId[g_iMAX_LEN];
char g_cFcmId[g_iMAX_LEN];
char g_cIbId[g_iMAX_LEN];
char g_cExchange[g_iMAX_LEN];
char g_cTradeRoute[g_iMAX_LEN];
RApi::AccountInfo g_oAccount;
tsNCharcb g_sExchange;
tsNCharcb g_sTradeRoute = {(char*)NULL, 0};

// Add with other global variables
bool g_bDone = false;

// Add these global variables after the existing globals

// Mutex and websocket related

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

int MyCallbacks::AccountList(AccountListInfo* pInfo, void* pContext, int* aiCode) {
    if (pInfo->iArrayLen > 0) {
        // Create JSON response for accounts
        std::stringstream json;
        json << "{\"type\":\"accounts\",\"accounts\":[";
        
        for (int i = 0; i < pInfo->iArrayLen; i++) {
            const auto& account = pInfo->asAccountInfoArray[i];
            
            // Create AccountData object
            AccountData accountData;
            accountData.account_id = std::string(account.sAccountId.pData, account.sAccountId.iDataLen);
            accountData.fcm_id = std::string(account.sFcmId.pData, account.sFcmId.iDataLen);
            accountData.ib_id = std::string(account.sIbId.pData, account.sIbId.iDataLen);
            
            g_AccountList.push_back(accountData);
            
            // Add account to JSON
            if (i > 0) json << ",";
            json << "{"
                 << "\"account_id\":\"" << accountData.account_id << "\","
                 << "\"fcm_id\":\"" << accountData.fcm_id << "\","
                 << "\"ib_id\":\"" << accountData.ib_id << "\""
                 << "}";
        }
        
        json << "]}";
        
        // Output JSON to stdout
        fprintf(stdout, "%s\n", json.str().c_str());
        fflush(stdout);
        
        DEBUG_PRINT("Processed %d accounts", pInfo->iArrayLen);
    } else {
        // Send empty account list
        fprintf(stdout, "{\"type\":\"accounts\",\"accounts\":[]}\n");
        fflush(stdout);
        DEBUG_PRINT("Warning: No accounts received");
    }
    
    g_bRcvdAccount = true;
    *aiCode = API_OK;
    return(OK);
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

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

int main(int argc, char** argv, char** envp) {
    char* USAGE = (char*)"GetAccountList user password server_type location\n";
    
    MyAdmCallbacks *  pAdmCallbacks;
    RCallbacks *      pCallbacks;
    REngineParams     oParams;
    LoginParams       oLoginParams;
    char *            fake_envp[9];
    int               iCode;

    /*   ----------------------------------------------------------------   */

    if (argc < 5)
         {
         DEBUG_PRINT("%s", USAGE);
         return (BAD);
         }

    /*   ----------------------------------------------------------------   */

    try {
        pAdmCallbacks = new MyAdmCallbacks();
    } catch (OmneException& oEx) {
        iCode = oEx.getErrorCode();
        DEBUG_PRINT("MyAdmCallbacks::MyAdmCallbacks() error : %d\n", iCode);
        return (BAD);
    }

    /*   ----------------------------------------------------------------   */
    /*   instantiate a callback object - prerequisite for logging in */
    try {
        pCallbacks = new MyCallbacks();
    } catch (OmneException& oEx) {
        delete pAdmCallbacks;

        iCode = oEx.getErrorCode();
        DEBUG_PRINT("MyCallbacks::MyCallbacks() error : %d\n", iCode);
        return (BAD);
    }

    /*   ----------------------------------------------------------------   */
    /*   Load server configuration based on server type and location   */

    // Load server configurations
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

    // Log server configuration details
    fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Server Configuration - Type: %s, Location: %s\"}\n", 
        argv[3], argv[4]);
    fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Domain Name: %s\"}\n", 
        server_json["MML_DOMAIN_NAME"].get<std::string>().c_str());
    fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"DMN Server: %s\"}\n", 
        server_json["MML_DMN_SRVR_ADDR"].get<std::string>().c_str());
    fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"License Server: %s\"}\n", 
        server_json["MML_LIC_SRVR_ADDR"].get<std::string>().c_str());
    fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Broker Address: %s\"}\n", 
        server_json["MML_LOC_BROK_ADDR"].get<std::string>().c_str());
    fprintf(stderr, "{\"type\":\"log\",\"level\":\"info\",\"message\":\"Logger Address: %s\"}\n", 
        server_json["MML_LOGGER_ADDR"].get<std::string>().c_str());

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

    // Log the environment variables for debugging
    DEBUG_PRINT("Environment variables:\n");
    for (int i = 0; fake_envp[i] != NULL; i++) {
        DEBUG_PRINT("%s\n", fake_envp[i]);
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

    /*   ----------------------------------------------------------------   */

    try {
        g_pEngine = new REngine(&oParams);
    } catch (OmneException& oEx) {
        delete pAdmCallbacks;
        delete pCallbacks;

        iCode = oEx.getErrorCode();
        DEBUG_PRINT("REngine::REngine() error : %d\n", iCode);
        return (BAD);
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

    // Log the connection points for debugging
    DEBUG_PRINT("Connection points:\n");
    DEBUG_PRINT("TS_CNNCT_PT: %s\n", ts_point.c_str());

    oLoginParams.sTsUser.pData = argv[1];
    oLoginParams.sTsUser.iDataLen = (int)strlen(oLoginParams.sTsUser.pData);

    oLoginParams.sTsPassword.pData = argv[2];
    oLoginParams.sTsPassword.iDataLen = (int)strlen(oLoginParams.sTsPassword.pData);

    /*   ----------------------------------------------------------------   */

    if (!g_pEngine -> login(&oLoginParams, &iCode))
         {
         DEBUG_PRINT("REngine::login() error : %d\n", iCode);

         delete g_pEngine;
         delete pCallbacks;
         delete pAdmCallbacks;

         return (BAD);
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

    // Send completion message
    fprintf(stdout, "{\"type\":\"complete\",\"total_accounts\":%zu}\n", g_AccountList.size());
    fflush(stdout);

    // Cleanup
    g_AccountList.clear();
    
    // Clean up dynamically allocated environment variables
    for (int i = 0; i < 8; i++) {
        delete[] fake_envp[i];
    }

    // Clean up connection point strings
    free((void*)oLoginParams.sTsCnnctPt.pData);
    
    return GOOD;
}

