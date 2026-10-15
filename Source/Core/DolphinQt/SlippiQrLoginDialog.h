#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <QDialog>

class QLabel;
class QPushButton;
class QTimer;

// QR sign-in: shows a QR code for the server's device-code page, polls until the
// phone approves, then writes user.json where the browser login would.
// Server contract: POST <origin>/api/device/code, then POST <origin>/api/device/token.
class SlippiQrLoginDialog final : public QDialog
{
  Q_OBJECT
public:
  SlippiQrLoginDialog(QWidget* parent, std::string origin, std::string account_path);
  ~SlippiQrLoginDialog() override;

  // Shows the one sign-in window (creating it, or restarting the open one).
  static void ShowFor(QWidget* parent, const std::string& origin, const std::string& account_path);

  // Asks for a new code (Retry, or the window being reopened by another Login).
  void Restart();

  enum class State
  {
    Loading,
    Showing,
    Approved,
    LoggedIn,
    Expired,
    Denied,
    Error,
  };

  struct CodeInfo
  {
    QString user_code;
    QString verification_uri;
    QString verification_uri_complete;
    int match_number = 0;
    int expires_in = 600;
  };

private:
  struct Attempt
  {
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> cancel{false};
  };

  void CreateWidgets();
  void StopAttempt();
  void Run(std::shared_ptr<Attempt> attempt);
  void PostState(const std::shared_ptr<Attempt>& attempt, State state, QString error = {});
  void PostCode(const std::shared_ptr<Attempt>& attempt, CodeInfo info);
  void PostUnsupported(const std::shared_ptr<Attempt>& attempt);
  void SetState(State state, const QString& error);
  void ShowCode(const CodeInfo& info);
  void UpdateCountdown();
  void ManualLogin();

  const std::string m_origin;
  const std::string m_account_path;

  std::shared_ptr<Attempt> m_attempt;
  std::thread m_thread;

  State m_state = State::Loading;
  qint64 m_deadline_ms = 0;
  int m_logged_in_ticks = 0;

  QLabel* m_qr = nullptr;
  QLabel* m_code = nullptr;
  QLabel* m_pick = nullptr;
  QLabel* m_title = nullptr;
  QLabel* m_body = nullptr;
  QLabel* m_address = nullptr;
  QLabel* m_countdown = nullptr;
  QPushButton* m_retry = nullptr;
  QPushButton* m_manual = nullptr;
  QPushButton* m_close = nullptr;
  QTimer* m_timer = nullptr;
};
