/**
 * @file TelnetServer.cpp
 * @brief TelnetServer: accept loop, sessions, IAC handling, "telnet" commands.
 */
#include "TelnetServer.h"

#include "Defaults.h"
#include "i18n/I18n.h"

#include <cerrno>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define LOG_TAG     "TelnetServer"
#define LOG_ENABLED 1
#include "Log.h"

namespace dflt = defaults::telnet; // factory settings of telnet.json

// Telnet protocol bytes.
static constexpr uint8_t IAC = 255;
static constexpr uint8_t DONT = 254;
static constexpr uint8_t WILL = 251;
static constexpr uint8_t SB = 250;
static constexpr uint8_t SE = 240;
static constexpr uint8_t OPT_ECHO = 1;
static constexpr uint8_t OPT_SGA = 3;

static constexpr int SESSION_STACK = 6144;
static constexpr int STOP_TIMEOUT_MS = 3000;

TelnetServer::TelnetServer(const CommandExecutor& executor, const GlobalSettings& global, WifiManager& wifi)
    : executor(executor),
      global(global),
      wifi(wifi)
{
}

void TelnetServer::loadConfig(ConfigReader& r)
{
    cfg.port = r.integer("port", dflt::PORT, dflt::PORT_MIN, dflt::PORT_MAX);
    cfg.maxSessions = r.integer("max_sessions", dflt::MAX_SESSIONS, dflt::MAX_SESSIONS_MIN, dflt::MAX_SESSIONS_MAX);
    cfg.idleTimeoutS = r.integer("idle_timeout_s", dflt::IDLE_TIMEOUT_S, dflt::IDLE_TIMEOUT_S_MIN, dflt::IDLE_TIMEOUT_S_MAX);
}

cJSON* TelnetServer::saveConfig() const
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "port", cfg.port);
    cJSON_AddNumberToObject(root, "max_sessions", cfg.maxSessions);
    cJSON_AddNumberToObject(root, "idle_timeout_s", cfg.idleTimeoutS);
    return root;
}

// Start with the current configuration (a service restart takes the changes from config set).
esp_err_t TelnetServer::start()
{
    if (running) {
        return ESP_OK;
    }
    port = cfg.port;
    maxSessions = cfg.maxSessions;
    idleTimeoutS = cfg.idleTimeoutS;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        LOG_E("socket() nie powiodl sie: errno %d", errno);
        return ESP_FAIL;
    }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    // Listen on all interfaces; access is checked after accept() (isAllowed).
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(sock, 2) != 0) {
        LOG_E("Nie udalo sie nasluchiwac na porcie %u: errno %d", port, errno);
        close(sock);
        return ESP_FAIL;
    }

    listenSock = sock;
    stopRequested = false;
    acceptAlive = true;
    if (xTaskCreate(&acceptTaskEntry, "telnet", 3072, this, 5, nullptr) != pdPASS) {
        acceptAlive = false;
        close(sock);
        listenSock = -1;
        return ESP_ERR_NO_MEM;
    }
    running = true;
    LOG_I("Telnet na porcie %u (max %d sesji, %s)", port, maxSessions,
          global.values().mgmtFromSta ? "dostep z AP i STA" : "dostep tylko z AP");
    return ESP_OK;
}

// Closes the listening socket and all sessions, waits for their tasks to finish.
esp_err_t TelnetServer::stop()
{
    if (!running) {
        return ESP_OK;
    }
    stopRequested = true;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (Session* s : sessions) {
            if (s != nullptr) {
                s->print(tr("\r\nSerwer Telnet zatrzymany\r\n", "\r\nTelnet server stopped\r\n"));
                shutdown(s->sock, SHUT_RDWR);
            }
        }
    }

    for (int waited = 0; waited < STOP_TIMEOUT_MS; waited += 20) {
        bool sessionsLeft = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (Session* s : sessions) {
                sessionsLeft = sessionsLeft || s != nullptr;
            }
        }
        if (!acceptAlive && !sessionsLeft) {
            running = false;
            LOG_I("Telnet zatrzymany");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    LOG_E("Telnet: zadania nie zakonczyly sie w %d ms", STOP_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

bool TelnetServer::owns(const IOutput& out) const
{
    std::lock_guard<std::mutex> lock(mutex);
    for (const Session* s : sessions) {
        if (s != nullptr && s == &out) {
            return true;
        }
    }
    return false;
}

void TelnetServer::describe(IOutput& out) const
{
    if (!running) {
        return;
    }
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const Session* s : sessions) {
            count += s != nullptr;
        }
    }
    out.printf(tr(" port %u, sesje %d/%d", " port %u, sessions %d/%d"), port, count, maxSessions);
}

void TelnetServer::acceptTaskEntry(void* arg)
{
    auto* self = static_cast<TelnetServer*>(arg);
    self->acceptLoop();
    close(self->listenSock);
    self->listenSock = -1;
    self->acceptAlive = false;
    vTaskDelete(nullptr);
}

// The local address of the connection tells the interface it came through: AP address = AP side.
bool TelnetServer::isAllowed(int sock) const
{
    if (global.values().mgmtFromSta) {
        return true;
    }
    sockaddr_in local = {};
    socklen_t len = sizeof(local);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
        return false;
    }
    esp_netif_ip_info_t ap = {};
    esp_netif_get_ip_info(wifi.apNetif(), &ap);
    return local.sin_addr.s_addr == ap.ip.addr;
}

void TelnetServer::acceptLoop()
{
    while (!stopRequested) {
        // select() with a timeout, so that the task can notice a stop request.
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listenSock, &fds);
        timeval tv = { 0, 200 * 1000 };
        if (select(listenSock + 1, &fds, nullptr, nullptr, &tv) <= 0) {
            continue;
        }

        sockaddr_in peer = {};
        socklen_t len = sizeof(peer);
        int sock = accept(listenSock, reinterpret_cast<sockaddr*>(&peer), &len);
        if (sock < 0) {
            LOG_W("accept() nie powiodl sie: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char peerStr[24];
        snprintf(peerStr, sizeof(peerStr), "%s:%u", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

        if (!isAllowed(sock)) {
            LOG_W("Odrzucono polaczenie Telnet z %s (dostep tylko od strony AP)", peerStr);
            const char* msg = tr("Dostęp tylko z sieci AP\r\n", "Access only from the AP network\r\n");
            send(sock, msg, strlen(msg), 0);
            close(sock);
            continue;
        }

        auto* s = new Session(*this, sock);
        strlcpy(s->peer, peerStr, sizeof(s->peer));
        s->connectedAt = esp_timer_get_time();
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (int i = 0; i < maxSessions; ++i) {
                if (sessions[i] == nullptr) {
                    sessions[i] = s;
                    s->slot = i;
                    break;
                }
            }
        }
        if (s->slot < 0) {
            LOG_W("Odrzucono polaczenie Telnet z %s - limit %d sesji", peerStr, maxSessions);
            s->print(tr("Za dużo sesji Telnet\r\n", "Too many Telnet sessions\r\n"));
            close(sock);
            delete s;
            continue;
        }

        if (xTaskCreate(&sessionTaskEntry, "telnet-s", SESSION_STACK, s, 5, nullptr) != pdPASS) {
            LOG_E("Brak pamieci na zadanie sesji Telnet");
            {
                std::lock_guard<std::mutex> lock(mutex);
                sessions[s->slot] = nullptr;
            }
            close(sock);
            delete s;
            continue;
        }
        LOG_I("Sesja Telnet %d: %s", s->slot, peerStr);
    }
}

void TelnetServer::sessionTaskEntry(void* arg)
{
    auto* s = static_cast<Session*>(arg);
    s->server.runSession(s);
    vTaskDelete(nullptr);
}

TelnetServer::Session::Session(TelnetServer& server, int sock)
    : server(server),
      sock(sock),
      editor(server.executor, *this, true)
{
}

// Sends everything, in ASCII by default (Polish characters without diacritics, as on COM; global.console_utf8).
// Errors (broken connection) are ignored – recv() in the session loop detects them.
void TelnetServer::Session::write(const char* data, size_t len)
{
    i18n::consoleWrite(data, len, [this](const char* d, size_t n) { sendRaw(d, n); });
}

// Byte 0xFF doubled as required by the Telnet protocol.
void TelnetServer::Session::sendRaw(const char* data, size_t len)
{
    const char* p = data;
    const char* end = data + len;
    while (p < end) {
        const char* iac = static_cast<const char*>(memchr(p, IAC, end - p));
        const char* chunkEnd = iac ? iac + 1 : end;
        while (p < chunkEnd) {
            int n = send(sock, p, chunkEnd - p, 0);
            if (n <= 0) {
                return;
            }
            p += n;
        }
        if (iac != nullptr) {
            uint8_t extra = IAC;
            send(sock, &extra, 1, 0);
        }
    }
}

// Skips Telnet control sequences (IAC ...), passes the rest to the line editor.
void TelnetServer::handleIncoming(Session* s, uint8_t c)
{
    switch (s->telnetState) {
    case 0: // ordinary data
        if (c == IAC) {
            s->telnetState = 1;
        } else {
            s->editor.handleChar(static_cast<char>(c));
        }
        break;
    case 1: // after IAC
        if (c == SB) {
            s->telnetState = 3;
        } else if (c >= WILL && c <= DONT) {
            s->telnetState = 2;
        } else {
            s->telnetState = 0; // IAC IAC or a one-byte command
        }
        break;
    case 2: // option after WILL/WONT/DO/DONT
        s->telnetState = 0;
        break;
    case 3: // SB ... IAC SE negotiation
        if (c == IAC) {
            s->telnetState = 4;
        }
        break;
    case 4:
        s->telnetState = (c == SE) ? 0 : 3;
        break;
    }
}

void TelnetServer::runSession(Session* s)
{
    int one = 1;
    setsockopt(s->sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int nodelay = 1;
    setsockopt(s->sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    if (idleTimeoutS > 0) {
        timeval tv = { static_cast<time_t>(idleTimeoutS), 0 };
        setsockopt(s->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    // The server does the echo and line editing, the client sends character by character.
    const uint8_t negotiate[] = { IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA };
    send(s->sock, negotiate, sizeof(negotiate), 0);

    const esp_app_desc_t* app = esp_app_get_description();
    s->printf(tr("\r\nEsp32Router %s – %s\r\nWpisz 'help', 'exit' kończy sesję.\r\n", "\r\nEsp32Router %s – %s\r\nType 'help', 'exit' ends the session.\r\n"), app->version,
              global.values().hostname.c_str());
    s->editor.prompt();

    uint8_t buf[64];
    while (!s->editor.closeRequested()) {
        int n = recv(s->sock, buf, sizeof(buf), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            s->printf(tr("\r\nBrak aktywności przez %lu s – rozłączam\r\n", "\r\nNo activity for %lu s – disconnecting\r\n"), (unsigned long)idleTimeoutS);
            break;
        }
        if (n <= 0) {
            break; // the client disconnected or the session was kicked (telnet kick)
        }
        for (int i = 0; i < n && !s->editor.closeRequested(); ++i) {
            handleIncoming(s, buf[i]);
        }
    }

    if (s->editor.closeRequested()) {
        s->print(tr("Do widzenia\r\n", "Goodbye\r\n"));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        sessions[s->slot] = nullptr;
    }
    close(s->sock);
    LOG_I("Sesja Telnet %d (%s) zakonczona", s->slot, s->peer);
    delete s;
}

void TelnetServer::registerCommands(CommandExecutor& exec)
{
    exec.addGroup("telnet", { "serwer Telnet", "Telnet server" });
    exec.add("telnet", { "sessions", "", { "lista sesji Telnet", "list of Telnet sessions" }, &cmdSessions, this });
    exec.add("telnet", { "kick", "<nr>", { "rozłącza sesję o podanym numerze", "disconnects the session with the given number" }, &cmdKick, this });
    exec.add(nullptr, { "exit", "", { "kończy sesję Telnet (także Ctrl+D)", "ends the Telnet session (also Ctrl+D)" }, &cmdExit, this });
}

CmdResult TelnetServer::cmdSessions(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<TelnetServer*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    if (!self->running) {
        out.print(tr("Telnet zatrzymany\r\n", "Telnet stopped\r\n"));
        return CmdResult::OK;
    }

    int64_t now = esp_timer_get_time();
    int count = 0;
    std::lock_guard<std::mutex> lock(self->mutex);
    for (int i = 0; i < self->maxSessions; ++i) {
        const Session* s = self->sessions[i];
        if (s == nullptr) {
            continue;
        }
        unsigned long secs = static_cast<unsigned long>((now - s->connectedAt) / 1000000);
        out.printf("  %d  %-22s  %lu:%02lu:%02lu%s\r\n", i, s->peer, secs / 3600, secs / 60 % 60, secs % 60,
                   s == &out ? tr("  (ta sesja)", "  (this session)") : "");
        ++count;
    }
    if (count == 0) {
        out.print(tr("Brak sesji\r\n", "No sessions\r\n"));
    }
    return CmdResult::OK;
}

CmdResult TelnetServer::cmdKick(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<TelnetServer*>(ctx);
    if (argc != 2) {
        return CmdResult::ERR_ARGS;
    }
    char* end = nullptr;
    long n = strtol(argv[1], &end, 10);
    if (*end != '\0' || n < 0 || n >= self->maxSessions) {
        return CmdResult::ERR_ARGS;
    }

    std::lock_guard<std::mutex> lock(self->mutex);
    Session* s = self->sessions[n];
    if (s == nullptr) {
        out.printf(tr("Brak sesji %ld\r\n", "No session %ld\r\n"), n);
        return CmdResult::ERR_FAIL;
    }
    // Shutting down the receive side interrupts recv() in the session task, which cleans up itself.
    // The session is freed only after removal from the table under the same mutex.
    shutdown(s->sock, SHUT_RDWR);
    out.printf(tr("Rozłączono sesję %ld (%s)\r\n", "Disconnected session %ld (%s)\r\n"), n, s->peer);
    return CmdResult::OK;
}

CmdResult TelnetServer::cmdExit(void*, int, char**, IOutput& out)
{
    // In a Telnet session "exit" is handled by LineEditor and does not get here.
    out.print(tr("'exit' dotyczy tylko sesji Telnet\r\n", "'exit' applies only to Telnet sessions\r\n"));
    return CmdResult::OK;
}
