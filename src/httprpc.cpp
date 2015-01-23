#include "httprpc.h"

#include "base58.h"
#include "chainparams.h"
#include "httpserver.h"
#include "rpcprotocol.h"
#include "rpcserver.h"
#include "random.h"
#include "sync.h"
#include "util.h"
#include "utilstrencodings.h"
#include "ui_interface.h"

#include <boost/algorithm/string.hpp> // boost::trim

/** Simple one-shot callback timer to be used by the RPC mechanism to e.g.
 * re-lock the wellet.
 */
class HTTPRPCTimer : public RPCTimerBase
{
public:
    HTTPRPCTimer(struct event_base* eventBase, boost::function<void(void)>& func, int64_t seconds) : ev(eventBase, false, new Handler(func))
    {
        struct timeval tv = {seconds, 0};
        ev.trigger(&tv);
    }
private:
    HTTPEvent ev;

    class Handler : public HTTPClosure
    {
    public:
        Handler(const boost::function<void(void)>& func) : func(func)
        {
        }
    private:
        boost::function<void(void)> func;
        void operator()() { func(); }
    };
};

class HTTPRPCTimerInterface : public RPCTimerInterface
{
public:
    HTTPRPCTimerInterface(struct event_base* base) : base(base)
    {
    }
    const char* Name()
    {
        return "HTTP";
    }
    RPCTimerBase* NewTimer(boost::function<void(void)>& func, int64_t seconds)
    {
        return new HTTPRPCTimer(base, func, seconds);
    }
private:
    struct event_base* base;
};


/* Pre-base64-encoded authentication token */
static std::string strRPCUserColonPass;
/* Stored RPC timer interface (for unregistration) */
static HTTPRPCTimerInterface* httpRPCTimerInterface = 0;

static void JSONErrorReply(HTTPRequest* req, const json_spirit::Object& objError, const json_spirit::Value& id)
{
    // Send error reply from json-rpc error object
    int nStatus = HTTP_INTERNAL_SERVER_ERROR;
    int code = find_value(objError, "code").get_int();

    if (code == RPC_INVALID_REQUEST)
        nStatus = HTTP_BAD_REQUEST;
    else if (code == RPC_METHOD_NOT_FOUND)
        nStatus = HTTP_NOT_FOUND;

    std::string strReply = JSONRPCReply(json_spirit::Value::null, objError, id);

    req->WriteHeader("Content-Type", "application/json");
    req->WriteReply(nStatus, strReply);
}

static bool RPCAuthorized(const std::string& strAuth)
{
    if (strRPCUserColonPass.empty()) // Belt-and-suspenders measure if InitRPCAuthentication was not called
        return false;
    if (strAuth.substr(0, 6) != "Basic ")
        return false;
    std::string strUserPass64 = strAuth.substr(6);
    boost::trim(strUserPass64);
    std::string strUserPass = DecodeBase64(strUserPass64);
    return TimingResistantEqual(strUserPass, strRPCUserColonPass);
}

static bool HTTPReq_JSONRPC(HTTPRequest* req, const std::string &)
{
    // JSONRPC handles only POST
    if (req->GetRequestMethod() != HTTPRequest::POST) {
        req->WriteReply(HTTP_BAD_METHOD, "JSONRPC server handles only POST requests");
        return false;
    }
    // Check authorization
    std::pair<bool, std::string> authHeader = req->GetHeader("authorization");
    if (!authHeader.first) {
        req->WriteReply(HTTP_UNAUTHORIZED);
        return false;
    }

    if (!RPCAuthorized(authHeader.second)) {
        LogPrintf("ThreadRPCServer incorrect password attempt from %s\n", req->GetPeer().ToString());

        /* Deter brute-forcing
           If this results in a DoS the user really
           shouldn't have their RPC port exposed. */
        MilliSleep(250);

        req->WriteReply(HTTP_UNAUTHORIZED);
        return false;
    }

    JSONRequest jreq;
    try {
        // Parse request
        std::string strRequest = req->ReadBody();
        json_spirit::Value valRequest;
        if (!read_string(strRequest, valRequest))
            throw JSONRPCError(RPC_PARSE_ERROR, "Parse error");

        // Return immediately if in warmup
        std::string warmupStatus;
        if (RPCIsInWarmup(&warmupStatus))
            throw JSONRPCError(RPC_IN_WARMUP, warmupStatus);

        std::string strReply;

        // singleton request
        if (valRequest.type() == json_spirit::obj_type) {
            jreq.parse(valRequest);

            json_spirit::Value result = tableRPC.execute(jreq.strMethod, jreq.params);

            // Send reply
            strReply = JSONRPCReply(result, json_spirit::Value::null, jreq.id);

            // array of requests
        } else if (valRequest.type() == json_spirit::array_type)
            strReply = JSONRPCExecBatch(valRequest.get_array());
        else
            throw JSONRPCError(RPC_PARSE_ERROR, "Top-level object parse error");

        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strReply);
    } catch (const json_spirit::Object& objError) {
        JSONErrorReply(req, objError, jreq.id);
        return false;
    } catch (const std::exception& e) {
        JSONErrorReply(req, JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
        return false;
    }
    return true;
}

static bool InitRPCAuthentication()
{
    strRPCUserColonPass = mapArgs["-rpcuser"] + ":" + mapArgs["-rpcpassword"];
    if (((mapArgs["-rpcpassword"] == "") ||
         (mapArgs["-rpcuser"] == mapArgs["-rpcpassword"])) &&
        Params().RequireRPCPassword()) {
        unsigned char rand_pwd[32];
        GetRandBytes(rand_pwd, 32);
        uiInterface.ThreadSafeMessageBox(strprintf(
                                             _("To use bitcoind, or the -server option to bitcoin-qt, you must set an rpcpassword in the configuration file:\n"
                                               "%s\n"
                                               "It is recommended you use the following random password:\n"
                                               "rpcuser=bitcoinrpc\n"
                                               "rpcpassword=%s\n"
                                               "(you do not need to remember this password)\n"
                                               "The username and password MUST NOT be the same.\n"
                                               "If the file does not exist, create it with owner-readable-only file permissions.\n"
                                               "It is also recommended to set alertnotify so you are notified of problems;\n"
                                               "for example: alertnotify=echo %%s | mail -s \"Bitcoin Alert\" admin@foo.com\n"),
                                             GetConfigFile().string(),
                                             EncodeBase58(&rand_pwd[0], &rand_pwd[0] + 32)),
                                         "", CClientUIInterface::MSG_ERROR | CClientUIInterface::SECURE);
        return false;
    }
    return true;
}

bool StartHTTPRPC()
{
    LogPrint("rpc", "Starting HTTP RPC server\n");
    if (!InitRPCAuthentication())
        return false;

    RegisterHTTPHandler("/", true, HTTPReq_JSONRPC);

    assert(EventBase());
    httpRPCTimerInterface = new HTTPRPCTimerInterface(EventBase());
    RPCRegisterTimerInterface(httpRPCTimerInterface);
    return true;
}

void InterruptHTTPRPC()
{
    LogPrint("rpc", "Interrupting HTTP RPC server\n");
}

void StopHTTPRPC()
{
    LogPrint("rpc", "Stopping HTTP RPC server\n");
    UnregisterHTTPHandler("/", true);
    if (httpRPCTimerInterface) {
        RPCUnregisterTimerInterface(httpRPCTimerInterface);
        delete httpRPCTimerInterface;
        httpRPCTimerInterface = 0;
    }
}
