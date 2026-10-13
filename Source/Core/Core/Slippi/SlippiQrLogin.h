#pragma once

#include <functional>
#include <string>

class SlippiUser;

// QR sign-in hook. When the game asks to log in and there is no user.json, the
// EXI device calls Start(). If a QR sign-in server is configured
// (Slippi.QrLoginServer) and the UI has registered a handler, the UI shows a QR
// code instead of the browser page. The UI writes user.json where the browser
// flow would, and the Rust user watcher picks it up as usual.
namespace SlippiQrLogin
{
// origin: the server's scheme://host[:port]; account_path: where user.json goes.
// Runs on the EXI thread; it must not block (post to the UI thread).
using Handler = std::function<void(const std::string& origin, const std::string& account_path)>;

void SetHandler(Handler handler);

// The EXI device registers its user so the UI can fall back to the browser flow.
void RegisterUser(SlippiUser* user);
void UnregisterUser(SlippiUser* user);

// Returns true when a QR sign-in was started; false means use the browser page.
bool Start();

// The configured server origin and the user.json path. False when QR sign-in is off.
bool GetTarget(std::string* origin, std::string* account_path);

// True while an EXI device is running (so the browser login can be opened).
bool CanOpenManualLogin();

// The browser login page (today's flow). Safe from any thread; does nothing once
// the EXI device is gone.
void OpenManualLogin();

// True once the Rust side has loaded a user. False when no device is running.
bool IsLoggedIn();

// The server origin (scheme://host[:port]) of a URL, or "" if it is not http(s).
std::string OriginOf(const std::string& url);
}  // namespace SlippiQrLogin
