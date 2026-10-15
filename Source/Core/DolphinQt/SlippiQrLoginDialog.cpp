#include "DolphinQt/SlippiQrLoginDialog.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <utility>
#include <vector>

#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <picojson.h>
#include <qrcodegen.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Core/Slippi/SlippiQrLogin.h"
#include "DolphinQt/QtUtils/SetWindowDecorations.h"

namespace
{
constexpr int QR_BOX_SIZE = 264;

const char* Platform()
{
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

const char* DeviceName()
{
#if defined(_WIN32)
  return "Slippi Dolphin on Windows";
#elif defined(__APPLE__)
  return "Slippi Dolphin on macOS";
#else
  return "Slippi Dolphin on Linux";
#endif
}

struct Reply
{
  bool reached = false;  // false: network error or cancelled
  int http = 0;
  std::string body;
};

bool ParseObject(const std::string& body, picojson::object& out)
{
  picojson::value root;
  if (!picojson::parse(root, body).empty() || !root.is<picojson::object>())
    return false;
  out = root.get<picojson::object>();
  return true;
}

std::string StringField(const picojson::object& obj, const char* key)
{
  const auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<std::string>())
    return "";
  return it->second.get<std::string>();
}

int IntField(const picojson::object& obj, const char* key, int fallback)
{
  const auto it = obj.find(key);
  if (it == obj.end())
    return fallback;
  if (it->second.is<double>())
    return static_cast<int>(it->second.get<double>());
  // The contract sends match_number as a zero-padded string ("47").
  if (it->second.is<std::string>())
  {
    const std::string& s = it->second.get<std::string>();
    if (!s.empty() && s.size() <= 4 && s.find_first_not_of("0123456789") == std::string::npos)
      return std::stoi(s);
  }
  return fallback;
}

// The same fields the Rust side needs to load user.json (user/src/lib.rs, UserInfo).
bool IsAccountFile(const std::string& body)
{
  picojson::object obj;
  if (!ParseObject(body, obj))
    return false;
  for (const char* key : {"uid", "playKey", "displayName", "connectCode"})
  {
    if (StringField(obj, key).empty())
      return false;
  }
  const auto it = obj.find("latestVersion");
  return it != obj.end() && it->second.is<std::string>();
}

// Writes user.json where the browser login would: a temp file, then a rename.
bool SaveAccount(const std::string& path, const std::string& body)
{
  if (!File::CreateFullPath(path))
    return false;
  const std::string tmp = path + ".qr-login.tmp";
  if (!File::WriteStringToFile(tmp, body))
    return false;
#ifndef _WIN32
  // The file holds a play key: owner only.
  std::error_code ec;
  std::filesystem::permissions(
      tmp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, ec);
#endif
  if (!File::Rename(tmp, path))
  {
    File::Delete(tmp);
    return false;
  }
  return true;
}

QPixmap RenderQr(const QString& text)
{
  std::vector<u8> qr(qrcodegen_BUFFER_LEN_FOR_VERSION(10));
  std::vector<u8> temp(qr.size());
  if (!qrcodegen_encodeText(text.toStdString().c_str(), temp.data(), qr.data(), qrcodegen_Ecc_MEDIUM,
                            1, 10, qrcodegen_Mask_AUTO, true))
  {
    return {};
  }
  const int size = qrcodegen_getSize(qr.data());
  constexpr int quiet = 4;
  const int modules = size + quiet * 2;
  const int scale = std::max(1, QR_BOX_SIZE / modules);
  QImage image(modules * scale, modules * scale, QImage::Format_RGB32);
  image.fill(Qt::white);
  for (int y = 0; y < size; y++)
  {
    for (int x = 0; x < size; x++)
    {
      if (!qrcodegen_getModule(qr.data(), x, y))
        continue;
      for (int dy = 0; dy < scale; dy++)
      {
        for (int dx = 0; dx < scale; dx++)
          image.setPixel((x + quiet) * scale + dx, (y + quiet) * scale + dy, qRgb(0, 0, 0));
      }
    }
  }
  return QPixmap::fromImage(image);
}
}  // namespace

SlippiQrLoginDialog::SlippiQrLoginDialog(QWidget* parent, std::string origin,
                                         std::string account_path)
    : QDialog(parent), m_origin(std::move(origin)), m_account_path(std::move(account_path))
{
  setWindowTitle(tr("Slippi Sign In"));
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
  CreateWidgets();

  m_timer = new QTimer(this);
  m_timer->setInterval(500);
  connect(m_timer, &QTimer::timeout, this, &SlippiQrLoginDialog::UpdateCountdown);
  m_timer->start();

  Restart();
}

void SlippiQrLoginDialog::ShowFor(QWidget* parent, const std::string& origin,
                                  const std::string& account_path)
{
  static QPointer<SlippiQrLoginDialog> s_dialog;
  if (!s_dialog)
  {
    s_dialog = new SlippiQrLoginDialog(parent, origin, account_path);
    s_dialog->setAttribute(Qt::WA_DeleteOnClose);
    SetQWidgetWindowDecorations(s_dialog);
  }
  else
  {
    s_dialog->Restart();
  }
  s_dialog->show();
  s_dialog->raise();
  s_dialog->activateWindow();
}

SlippiQrLoginDialog::~SlippiQrLoginDialog()
{
  StopAttempt();
}

void SlippiQrLoginDialog::CreateWidgets()
{
  m_qr = new QLabel();
  m_qr->setFixedSize(QR_BOX_SIZE, QR_BOX_SIZE);
  m_qr->setAlignment(Qt::AlignCenter);
  m_qr->setFrameShape(QFrame::StyledPanel);
  m_qr->setWordWrap(true);

  m_code = new QLabel();
  QFont code_font = m_code->font();
  code_font.setPointSizeF(code_font.pointSizeF() * 1.8);
  code_font.setBold(true);
  code_font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
  m_code->setFont(code_font);
  m_code->setAlignment(Qt::AlignCenter);
  m_code->setTextInteractionFlags(Qt::TextSelectableByMouse);

  m_pick = new QLabel();
  m_pick->setAlignment(Qt::AlignCenter);

  auto* left = new QVBoxLayout();
  left->addWidget(m_qr, 0, Qt::AlignHCenter);
  left->addWidget(m_code);
  left->addWidget(m_pick);
  left->addStretch();

  m_title = new QLabel();
  QFont title_font = m_title->font();
  title_font.setPointSizeF(title_font.pointSizeF() * 1.4);
  title_font.setBold(true);
  m_title->setFont(title_font);
  m_title->setWordWrap(true);

  m_body = new QLabel();
  m_body->setWordWrap(true);

  m_address = new QLabel();
  m_address->setWordWrap(true);
  m_address->setTextFormat(Qt::RichText);
  m_address->setOpenExternalLinks(true);
  m_address->setTextInteractionFlags(Qt::TextBrowserInteraction);

  m_countdown = new QLabel();
  m_countdown->setEnabled(false);  // the palette's gray

  auto* right = new QVBoxLayout();
  right->addWidget(m_title);
  right->addSpacing(6);
  right->addWidget(m_body);
  right->addSpacing(12);
  right->addWidget(m_address);
  right->addSpacing(12);
  right->addWidget(m_countdown);
  right->addStretch();

  auto* panes = new QHBoxLayout();
  panes->addLayout(left);
  panes->addSpacing(16);
  panes->addLayout(right, 1);

  m_retry = new QPushButton(tr("Retry"));
  m_manual = new QPushButton(tr("Manual login"));
  m_manual->setToolTip(tr("Log in through the browser instead."));
  m_close = new QPushButton(tr("Cancel"));
  m_retry->setAutoDefault(false);
  m_manual->setAutoDefault(false);
  m_close->setAutoDefault(false);

  auto* buttons = new QHBoxLayout();
  buttons->addWidget(m_retry);
  buttons->addWidget(m_manual);
  buttons->addStretch();
  buttons->addWidget(m_close);

  auto* main = new QVBoxLayout();
  main->addLayout(panes);
  main->addSpacing(8);
  main->addLayout(buttons);
  setLayout(main);
  setMinimumWidth(620);

  connect(m_retry, &QPushButton::clicked, this, &SlippiQrLoginDialog::Restart);
  connect(m_manual, &QPushButton::clicked, this, &SlippiQrLoginDialog::ManualLogin);
  connect(m_close, &QPushButton::clicked, this, &QDialog::reject);
}

void SlippiQrLoginDialog::StopAttempt()
{
  if (m_attempt)
  {
    {
      // Under the lock, so the worker cannot miss the wakeup between its check and its wait.
      std::lock_guard lock(m_attempt->m);
      m_attempt->cancel = true;
    }
    m_attempt->cv.notify_all();
  }
  // The progress callback aborts an in-flight request, so this is quick.
  if (m_thread.joinable())
    m_thread.join();
  m_attempt.reset();
}

void SlippiQrLoginDialog::Restart()
{
  StopAttempt();
  m_logged_in_ticks = 0;
  SetState(State::Loading, {});
  m_attempt = std::make_shared<Attempt>();
  m_thread = std::thread(&SlippiQrLoginDialog::Run, this, m_attempt);
}

void SlippiQrLoginDialog::ManualLogin()
{
  StopAttempt();
  SlippiQrLogin::OpenManualLogin();
  accept();
}

void SlippiQrLoginDialog::PostState(const std::shared_ptr<Attempt>& attempt, State state,
                                    QString error)
{
  // Queued to the UI thread; dropped if the dialog is gone or the attempt replaced.
  QMetaObject::invokeMethod(
      this,
      [this, attempt, state, error] {
        if (attempt == m_attempt)
          SetState(state, error);
      },
      Qt::QueuedConnection);
}

void SlippiQrLoginDialog::PostCode(const std::shared_ptr<Attempt>& attempt, CodeInfo info)
{
  QMetaObject::invokeMethod(
      this,
      [this, attempt, info] {
        if (attempt != m_attempt)
          return;
        ShowCode(info);
        SetState(State::Showing, {});
      },
      Qt::QueuedConnection);
}

void SlippiQrLoginDialog::PostUnsupported(const std::shared_ptr<Attempt>& attempt)
{
  // 404 from /api/device/code: this server has no QR sign-in; use today's flow.
  QMetaObject::invokeMethod(
      this,
      [this, attempt] {
        if (attempt != m_attempt)
          return;
        if (SlippiQrLogin::CanOpenManualLogin())
          ManualLogin();
        else
          SetState(State::Error, tr("This server has no QR sign-in. Use the browser login."));
      },
      Qt::QueuedConnection);
}

// Worker thread. The device code is a secret: it stays in this function's locals
// and is never shown, logged or posted to the UI.
void SlippiQrLoginDialog::Run(std::shared_ptr<Attempt> attempt)
{
  const auto wait = [&attempt](std::chrono::milliseconds duration) {
    std::unique_lock lock(attempt->m);
    return !attempt->cv.wait_for(lock, duration, [&attempt] { return attempt->cancel.load(); });
  };
  const auto post = [&attempt](const std::string& url, const std::string& json) {
    Reply reply;
    // No redirects: HttpRequest follows them only after FollowRedirects(), which is
    // not called here (libcurl's default is off). The token body holds the secret.
    Common::HttpRequest request(std::chrono::seconds(8),
                                [&attempt](s64, s64, s64, s64) { return !attempt->cancel.load(); });
    const auto response = request.Post(
        url, json,
        {{"Content-Type", "application/json"}, {"Accept", "application/json"}},
        Common::HttpRequest::AllowedReturnCodes::All);
    if (!response)
      return reply;
    reply.reached = true;
    reply.http = request.GetLastResponseCode();
    reply.body.assign(response->begin(), response->end());
    return reply;
  };

  std::string device_code;
  int interval = 5;
  int expires_in = 600;
  {
    const std::string request = std::string("{\"client\":\"slippi-dolphin\",\"platform\":\"") +
                                Platform() + "\",\"name\":\"" + DeviceName() + "\"}";
    const Reply r = post(m_origin + "/api/device/code", request);
    if (attempt->cancel)
      return;
    if (r.reached && r.http == 404)
    {
      INFO_LOG_FMT(SLIPPI_ONLINE, "[User] No QR sign-in at {}; using the browser login", m_origin);
      PostUnsupported(attempt);
      return;
    }
    if (!r.reached)
    {
      PostState(attempt, State::Error, tr("Could not reach the server."));
      return;
    }
    if (r.http == 429)
    {
      PostState(attempt, State::Error, tr("Too many codes. Try again in a few minutes."));
      return;
    }
    picojson::object root;
    if (r.http != 200 || !ParseObject(r.body, root))
    {
      PostState(attempt, State::Error, tr("Server error (%1).").arg(r.http));
      return;
    }
    device_code = StringField(root, "device_code");
    CodeInfo info;
    info.user_code = QString::fromStdString(StringField(root, "user_code"));
    info.verification_uri = QString::fromStdString(StringField(root, "verification_uri"));
    info.verification_uri_complete =
        QString::fromStdString(StringField(root, "verification_uri_complete"));
    info.match_number = IntField(root, "match_number", -1);
    expires_in = std::clamp(IntField(root, "expires_in", 600), 10, 3600);
    interval = std::clamp(IntField(root, "interval", 5), 1, 30);
    info.expires_in = expires_in;
    if (device_code.empty() || info.user_code.isEmpty() ||
        info.verification_uri_complete.isEmpty() || info.match_number < 0 ||
        info.match_number > 99)
    {
      PostState(attempt, State::Error, tr("The server sent an unusable code."));
      return;
    }
    PostCode(attempt, info);
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(expires_in);
  const std::string token_request = "{\"device_code\":\"" + device_code + "\"}";
  int failures = 0;
  for (;;)
  {
    if (!wait(std::chrono::seconds(interval)))
      return;
    if (std::chrono::steady_clock::now() >= deadline)
    {
      PostState(attempt, State::Expired);
      return;
    }
    const Reply r = post(m_origin + "/api/device/token", token_request);
    if (attempt->cancel)
      return;
    if (!r.reached || r.http >= 500)
    {
      if (++failures >= 6)
      {
        PostState(attempt, State::Error, tr("Lost contact with the server."));
        return;
      }
      continue;
    }
    failures = 0;

    if (r.http == 200)
    {
      if (!IsAccountFile(r.body))
      {
        PostState(attempt, State::Error, tr("The server sent an invalid account."));
        return;
      }
      if (!SaveAccount(m_account_path, r.body))
      {
        PostState(attempt, State::Error, tr("Could not save the account file."));
        return;
      }
      PostState(attempt, State::Approved);
      // The Rust watcher loads user.json on its own schedule; wait for it, but
      // the file is saved either way.
      for (int i = 0; i < 40 && !SlippiQrLogin::IsLoggedIn(); i++)
      {
        if (!wait(std::chrono::milliseconds(250)))
          return;
      }
      if (!wait(std::chrono::milliseconds(1500)))  // let "Approved!" be read
        return;
      PostState(attempt, State::LoggedIn);
      return;
    }

    picojson::object root;
    const std::string error = ParseObject(r.body, root) ? StringField(root, "error") : "";
    if (error == "authorization_pending")
      continue;
    if (error == "slow_down")
    {
      interval = std::min(interval + 5, 60);
      continue;
    }
    if (error == "expired_token" || error == "invalid_grant")
    {
      PostState(attempt, State::Expired);
      return;
    }
    if (error == "access_denied")
    {
      PostState(attempt, State::Denied);
      return;
    }
    if (++failures >= 6)
    {
      PostState(attempt, State::Error, tr("The server refused the request."));
      return;
    }
  }
}

void SlippiQrLoginDialog::ShowCode(const CodeInfo& info)
{
  m_qr->setPixmap(RenderQr(info.verification_uri_complete));
  m_code->setText(info.user_code);
  m_pick->setText(tr("Pick <b>%1</b> on your phone").arg(info.match_number, 2, 10, QLatin1Char('0')));
  const QString uri =
      info.verification_uri.isEmpty() ? info.verification_uri_complete : info.verification_uri;
  m_address->setText(tr("No camera? Open <a href=\"%1\">%2</a> and enter the code.")
                         .arg(info.verification_uri_complete.toHtmlEscaped(), uri.toHtmlEscaped()));
  m_deadline_ms = QDateTime::currentMSecsSinceEpoch() + qint64(info.expires_in) * 1000;
}

void SlippiQrLoginDialog::SetState(State state, const QString& error)
{
  m_state = state;
  const bool code_visible = state == State::Showing || state == State::Approved;
  const bool failed = state == State::Expired || state == State::Denied || state == State::Error;

  if (!code_visible)
  {
    m_qr->setPixmap({});
    m_code->clear();
    m_pick->clear();
    m_address->clear();
  }
  m_pick->setVisible(state == State::Showing);
  m_address->setVisible(state == State::Showing);
  m_countdown->setVisible(state == State::Showing);

  switch (state)
  {
  case State::Loading:
    m_qr->setText(tr("Getting a code..."));
    m_title->setText(tr("Sign in with your phone"));
    m_body->setText(tr("Hang on."));
    break;
  case State::Showing:
    m_title->setText(tr("Scan with your phone and approve"));
    m_body->setText(tr("Point your phone's camera at the code, sign in, then pick the number "
                       "shown here."));
    break;
  case State::Approved:
    m_title->setText(tr("Approved!"));
    m_body->setText(tr("Signing you in..."));
    break;
  case State::LoggedIn:
    m_qr->setText(tr("Logged in"));
    m_title->setText(tr("Logged in"));
    m_body->setText(tr("You are signed in. This window closes by itself."));
    break;
  case State::Expired:
    m_qr->setText(tr("Code expired"));
    m_title->setText(tr("Code expired"));
    m_body->setText(tr("That code is no longer valid."));
    break;
  case State::Denied:
    m_qr->setText(tr("Sign-in denied"));
    m_title->setText(tr("Sign-in denied"));
    m_body->setText(tr("The request was not approved."));
    break;
  case State::Error:
    m_qr->setText(tr("Something went wrong"));
    m_title->setText(tr("Something went wrong"));
    m_body->setText(error.isEmpty() ? tr("The sign-in failed.") : error);
    break;
  }

  m_retry->setVisible(failed);
  // Manual login needs the running game (it opens the browser page for its user.json).
  m_manual->setVisible(state != State::LoggedIn && SlippiQrLogin::CanOpenManualLogin());
  m_close->setText(state == State::LoggedIn ? tr("Close") : tr("Cancel"));
  if (failed)
    m_retry->setFocus();
  UpdateCountdown();
}

void SlippiQrLoginDialog::UpdateCountdown()
{
  if (m_state == State::LoggedIn)
  {
    // About 3 seconds of "Logged in", then close.
    if (++m_logged_in_ticks >= 6)
      accept();
    return;
  }
  if (m_state != State::Showing)
    return;
  const qint64 left = std::max<qint64>(0, (m_deadline_ms - QDateTime::currentMSecsSinceEpoch()) / 1000);
  m_countdown->setText(
      tr("Expires in %1:%2").arg(left / 60).arg(left % 60, 2, 10, QLatin1Char('0')));
}
