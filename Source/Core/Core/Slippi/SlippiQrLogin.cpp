#include "Core/Slippi/SlippiQrLogin.h"

#include <mutex>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/Slippi/SlippiUser.h"

namespace SlippiQrLogin
{
namespace
{
std::mutex s_mutex;
Handler s_handler;
SlippiUser* s_user = nullptr;
}  // namespace

void SetHandler(Handler handler)
{
  std::lock_guard lock(s_mutex);
  s_handler = std::move(handler);
}

void RegisterUser(SlippiUser* user)
{
  std::lock_guard lock(s_mutex);
  s_user = user;
}

void UnregisterUser(SlippiUser* user)
{
  std::lock_guard lock(s_mutex);
  if (s_user == user)
    s_user = nullptr;
}

std::string OriginOf(const std::string& url)
{
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos)
    return "";
  const std::string scheme = url.substr(0, scheme_end);
  if (scheme != "http" && scheme != "https")
    return "";
  const size_t host_start = scheme_end + 3;
  const size_t host_end = url.find_first_of("/?#", host_start);
  const std::string host = url.substr(host_start, host_end == std::string::npos ?
                                                      std::string::npos :
                                                      host_end - host_start);
  if (host.empty() || host.find('@') != std::string::npos)
    return "";
  return scheme + "://" + host;
}

bool GetTarget(std::string* origin, std::string* account_path)
{
  *origin = OriginOf(Config::Get(Config::SLIPPI_QR_LOGIN_SERVER));
  *account_path = File::GetUserPath(D_SLIPPI_IDX) + "user.json";
  return !origin->empty();
}

bool CanOpenManualLogin()
{
  std::lock_guard lock(s_mutex);
  return s_user != nullptr;
}

bool Start()
{
  std::string origin, account_path;
  if (!GetTarget(&origin, &account_path))
    return false;

  Handler handler;
  {
    std::lock_guard lock(s_mutex);
    handler = s_handler;
  }
  if (!handler)
    return false;

  INFO_LOG_FMT(SLIPPI_ONLINE, "[User] QR sign-in at {}", origin);
  handler(origin, account_path);
  return true;
}

void OpenManualLogin()
{
  std::lock_guard lock(s_mutex);
  if (s_user)
    s_user->OpenLogInPage();
}

bool IsLoggedIn()
{
  std::lock_guard lock(s_mutex);
  return s_user && s_user->IsLoggedIn();
}
}  // namespace SlippiQrLogin
