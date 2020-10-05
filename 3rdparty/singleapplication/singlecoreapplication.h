// The MIT License (MIT)
//
// Copyright (c) Itay Grudev 2015 - 2020
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

//
//  W A R N I N G !!!
//  -----------------
//
// This is a modified version of SingleApplication,
// The original version is at:
//
// https://github.com/itay-grudev/SingleApplication
//
//

#ifndef SINGLECOREAPPLICATION_H
#define SINGLECOREAPPLICATION_H

#include <QtGlobal>
#include <QCoreApplication>
#include <QFlags>
#include <QByteArray>

class SingleCoreApplicationPrivate;

/**
 * @brief The SingleCoreApplication class handles multipe instances of the same Application
 * @see QCoreApplication
 */
class SingleCoreApplication : public QCoreApplication {
  Q_OBJECT

  typedef QCoreApplication app_t;

 public:
  /**
   * @brief Mode of operation of SingleCoreApplication.
   * Whether the block should be user-wide or system-wide and whether the
   * primary instance should be notified when a secondary instance had been
   * started.
   * @note Operating system can restrict the shared memory blocks to the same
   * user, in which case the User/System modes will have no effect and the
   * block will be user wide.
   * @enum
   */
  enum Mode {
    User = 1 << 0,
    System = 1 << 1,
    SecondaryNotification = 1 << 2,
    ExcludeAppVersion = 1 << 3,
    ExcludeAppPath = 1 << 4
  };
  Q_DECLARE_FLAGS(Options, Mode)

  /**
   * @brief Intitializes a SingleCoreApplication instance with argc command line
   * arguments in argv
   * @arg {int &} argc - Number of arguments in argv
   * @arg {const char *[]} argv - Supplied command line arguments
   * @arg {bool} allowSecondary - Whether to start the instance as secondary
   * if there is already a primary instance.
   * @arg {Mode} mode - Whether for the SingleCoreApplication block to be applied
   * User wide or System wide.
   * @arg {int} timeout - Timeout to wait in milliseconds.
   * @note argc and argv may be changed as Qt removes arguments that it
   * recognizes
   * @note Mode::SecondaryNotification only works if set on both the primary
   * instance and the secondary instance.
   * @note The timeout is just a hint for the maximum time of blocking
   * operations. It does not guarantee that the SingleCoreApplication
   * initialisation will be completed in given time, though is a good hint.
   * Usually 4*timeout would be the worst case (fail) scenario.
   */
  explicit SingleCoreApplication(int &argc, char *argv[], const bool allowSecondary = false, const Options options = Mode::User, const int timeout = 1000);
  ~SingleCoreApplication() override;

  /**
   * @brief Returns if the instance is the primary instance
   * @returns {bool}
   */
  bool isPrimary();

  /**
   * @brief Returns if the instance is a secondary instance
   * @returns {bool}
   */
  bool isSecondary();

  /**
   * @brief Returns a unique identifier for the current instance
   * @returns {qint32}
   */
  quint32 instanceId();

  /**
   * @brief Returns the process ID (PID) of the primary instance
   * @returns {qint64}
   */
  qint64 primaryPid();

  /**
   * @brief Returns the username of the user running the primary instance
   * @returns {QString}
   */
  QString primaryUser();

  /**
   * @brief Returns the username of the current user
   * @returns {QString}
   */
  QString currentUser();

  /**
   * @brief Sends a message to the primary instance. Returns true on success.
   * @param {int} timeout - Timeout for connecting
   * @returns {bool}
   * @note sendMessage() will return false if invoked from the primary
   * instance.
   */
  bool sendMessage(QByteArray message, int timeout = 1000);

 signals:
  void instanceStarted();
  void receivedMessage(quint32 instanceId, QByteArray message);

 private:
  SingleCoreApplicationPrivate *d_ptr;
  Q_DECLARE_PRIVATE(SingleCoreApplication)
  void abortSafely();
};

Q_DECLARE_OPERATORS_FOR_FLAGS(SingleCoreApplication::Options)

#endif  // SINGLECOREAPPLICATION_H
