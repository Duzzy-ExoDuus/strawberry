/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018-2019, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <functional>

#include <QtGlobal>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QDateTime>
#include <QString>
#include <QUrl>
#include <QImage>
#include <QtDebug>

#include "core/logging.h"
#include "core/taskmanager.h"
#include "core/musicstorage.h"
#include "core/tagreaderclient.h"
#include "core/song.h"
#include "organize.h"
#ifdef HAVE_GSTREAMER
#  include "transcoder/transcoder.h"
#endif

class OrganizeFormat;

using std::placeholders::_1;

const int Organize::kBatchSize = 10;
#ifdef HAVE_GSTREAMER
const int Organize::kTranscodeProgressInterval = 500;
#endif

Organize::Organize(TaskManager *task_manager, std::shared_ptr<MusicStorage> destination, const OrganizeFormat &format, bool copy, bool overwrite, bool mark_as_listened, bool albumcover, const NewSongInfoList &songs_info, bool eject_after, const QString &playlist)
    : thread_(nullptr),
      task_manager_(task_manager),
#ifdef HAVE_GSTREAMER
      transcoder_(new Transcoder(this)),
#endif
      destination_(destination),
      format_(format),
      copy_(copy),
      overwrite_(overwrite),
      mark_as_listened_(mark_as_listened),
      albumcover_(albumcover),
      eject_after_(eject_after),
      task_count_(songs_info.count()),
      playlist_(playlist),
      tasks_complete_(0),
      started_(false),
      task_id_(0),
      current_copy_progress_(0){

  original_thread_ = thread();

  for (const NewSongInfo &song_info : songs_info) {
    tasks_pending_ << Task(song_info);
  }

}

Organize::~Organize() {
  if (thread_) {
    thread_->quit();
    thread_->deleteLater();
  }
}

void Organize::Start() {

  if (thread_) return;

  task_id_ = task_manager_->StartTask(tr("Organizing files"));
  task_manager_->SetTaskBlocksCollectionScans(true);

  thread_ = new QThread;
  connect(thread_, SIGNAL(started()), SLOT(ProcessSomeFiles()));
#ifdef HAVE_GSTREAMER
  connect(transcoder_, SIGNAL(JobComplete(QString, QString, bool)), SLOT(FileTranscoded(QString, QString, bool)));
  connect(transcoder_, SIGNAL(LogLine(QString)), SLOT(LogLine(QString)));
#endif

  moveToThread(thread_);
  thread_->start();

}

void Organize::ProcessSomeFiles() {

  if (!started_) {
    if (!destination_->StartCopy(&supported_filetypes_)) {
      // Failed to start - mark everything as failed :(
      for (const Task &task : tasks_pending_) files_with_errors_ << task.song_info_.song_.url().toLocalFile();
      tasks_pending_.clear();
    }
    started_ = true;
  }

  // None left?
  if (tasks_pending_.isEmpty()) {
#ifdef HAVE_GSTREAMER
    if (!tasks_transcoding_.isEmpty()) {
      // Just wait - FileTranscoded will start us off again in a little while
      qLog(Debug) << "Waiting for transcoding jobs";
      transcode_progress_timer_.start(kTranscodeProgressInterval, this);
      return;
    }
#endif

    UpdateProgress();

    destination_->FinishCopy(files_with_errors_.isEmpty());
    if (eject_after_) destination_->Eject();

    task_manager_->SetTaskFinished(task_id_);

    emit Finished(files_with_errors_, log_);

    // Move back to the original thread so deleteLater() can get called in the main thread's event loop
    moveToThread(original_thread_);
    deleteLater();

    // Stop this thread
    thread_->quit();
    return;
  }

  // We process files in batches so we can be cancelled part-way through.
  for (int i = 0; i < kBatchSize; ++i) {
    SetSongProgress(0);

    if (tasks_pending_.isEmpty()) break;

    Task task = tasks_pending_.takeFirst();
    qLog(Info) << "Processing" << task.song_info_.song_.url().toLocalFile();

    // Use a Song instead of a tag reader
    Song song = task.song_info_.song_;
    if (!song.is_valid()) continue;

    // Get embedded album cover
    QImage cover = TagReaderClient::Instance()->LoadEmbeddedArtBlocking(task.song_info_.song_.url().toLocalFile());
    if (!cover.isNull()) song.set_image(cover);

#ifdef HAVE_GSTREAMER
    // Maybe this file is one that's been transcoded already?
    if (!task.transcoded_filename_.isEmpty()) {
      qLog(Debug) << "This file has already been transcoded";

      // Set the new filetype on the song so the formatter gets it right
      song.set_filetype(task.new_filetype_);

      // Fiddle the filename extension as well to match the new type
      song.set_url(QUrl::fromLocalFile(QFileInfo(song.basefilename()).completeBaseName() + "." + task.new_extension_));
      song.set_basefilename(QFileInfo(song.basefilename()).completeBaseName() + "." + task.new_extension_);
      task.song_info_.new_filename_ = QFileInfo(task.song_info_.new_filename_).completeBaseName() + "." + task.new_extension_;

      // Have to set this to the size of the new file or else funny stuff happens
      song.set_filesize(QFileInfo(task.transcoded_filename_).size());
    }
    else {
      // Figure out if we need to transcode it
      Song::FileType dest_type = CheckTranscode(song.filetype());
      if (dest_type != Song::FileType_Unknown) {
        // Get the preset
        TranscoderPreset preset = Transcoder::PresetForFileType(dest_type);
        qLog(Debug) << "Transcoding with" << preset.name_;

        task.transcoded_filename_ = transcoder_->GetFile(task.song_info_.song_.url().toLocalFile(), preset);
        task.new_extension_ = preset.extension_;
        task.new_filetype_ = dest_type;
        tasks_transcoding_[task.song_info_.song_.url().toLocalFile()] = task;
        qLog(Debug) << "Transcoding to" << task.transcoded_filename_;

        // Start the transcoding - this will happen in the background and FileTranscoded() will get called when it's done.
	// At that point the task will get re-added to the pending queue with the new filename.
        transcoder_->AddJob(task.song_info_.song_.url().toLocalFile(), preset, task.transcoded_filename_);
        transcoder_->Start();
        continue;
      }
    }
#endif

    MusicStorage::CopyJob job;
    job.source_ = task.transcoded_filename_.isEmpty() ? task.song_info_.song_.url().toLocalFile() : task.transcoded_filename_;
    job.destination_ = task.song_info_.new_filename_;
    job.metadata_ = song;
    job.overwrite_ = overwrite_;
    job.mark_as_listened_ = mark_as_listened_;
    job.albumcover_ = albumcover_;
    job.remove_original_ = !copy_;
    job.playlist_ = playlist_;

    if (task.song_info_.song_.art_manual_is_valid() && task.song_info_.song_.art_manual().path() != Song::kManuallyUnsetCover) {
      if (task.song_info_.song_.art_manual().isLocalFile() && QFile::exists(task.song_info_.song_.art_manual().toLocalFile())) {
        job.cover_source_ = task.song_info_.song_.art_manual().toLocalFile();
      }
      else if (task.song_info_.song_.art_manual().scheme().isEmpty() && QFile::exists(task.song_info_.song_.art_manual().path())) {
        job.cover_source_ = task.song_info_.song_.art_manual().path();
      }
    }
    else if (task.song_info_.song_.art_automatic_is_valid() && task.song_info_.song_.art_automatic().path() != Song::kEmbeddedCover) {
      if (task.song_info_.song_.art_automatic().isLocalFile() && QFile::exists(task.song_info_.song_.art_automatic().toLocalFile())) {
        job.cover_source_ = task.song_info_.song_.art_automatic().toLocalFile();
      }
      else if (task.song_info_.song_.art_automatic().scheme().isEmpty() && QFile::exists(task.song_info_.song_.art_automatic().path())) {
        job.cover_source_ = task.song_info_.song_.art_automatic().path();
      }
    }
    if (!job.cover_source_.isEmpty()) {
      job.cover_dest_ = QFileInfo(job.destination_).path() + "/" + QFileInfo(job.cover_source_).fileName();
    }

    job.progress_ = std::bind(&Organize::SetSongProgress, this, _1, !task.transcoded_filename_.isEmpty());

    if (!destination_->CopyToStorage(job)) {
      files_with_errors_ << task.song_info_.song_.basefilename();
    }
    else {
      if (job.remove_original_) {
        // Notify other aspects of system that song has been invalidated
        QString root = destination_->LocalPath();
        QFileInfo new_file = QFileInfo(root + "/" + task.song_info_.new_filename_);
        emit SongPathChanged(song, new_file);
      }
      if (job.mark_as_listened_) {
        emit FileCopied(job.metadata_.id());
      }
    }

    // Clean up the temporary transcoded file
    if (!task.transcoded_filename_.isEmpty())
      QFile::remove(task.transcoded_filename_);

    tasks_complete_++;
  }
  SetSongProgress(0);

  QTimer::singleShot(0, this, SLOT(ProcessSomeFiles()));

}

#ifdef HAVE_GSTREAMER
Song::FileType Organize::CheckTranscode(Song::FileType original_type) const {

  if (original_type == Song::FileType_Stream) return Song::FileType_Unknown;

  const MusicStorage::TranscodeMode mode = destination_->GetTranscodeMode();
  const Song::FileType format = destination_->GetTranscodeFormat();

  switch (mode) {
    case MusicStorage::Transcode_Never:
      return Song::FileType_Unknown;

    case MusicStorage::Transcode_Always:
      if (original_type == format) return Song::FileType_Unknown;
      return format;

    case MusicStorage::Transcode_Unsupported:
      if (supported_filetypes_.isEmpty() || supported_filetypes_.contains(original_type)) return Song::FileType_Unknown;

      if (format != Song::FileType_Unknown) return format;

      // The user hasn't visited the device properties page yet to set a preferred format for the device, so we have to pick the best available one.
      return Transcoder::PickBestFormat(supported_filetypes_);
  }
  return Song::FileType_Unknown;

}
#endif

void Organize::SetSongProgress(float progress, bool transcoded) {

  const int max = transcoded ? 50 : 100;
  current_copy_progress_ = (transcoded ? 50 : 0) + qBound(0, static_cast<int>(progress * max), max - 1);
  UpdateProgress();

}

void Organize::UpdateProgress() {

  const int total = task_count_ * 100;

#ifdef HAVE_GSTREAMER
  // Update transcoding progress
  QMap<QString, float> transcode_progress = transcoder_->GetProgress();
  for (const QString &filename : transcode_progress.keys()) {
    if (!tasks_transcoding_.contains(filename)) continue;
    tasks_transcoding_[filename].transcode_progress_ = transcode_progress[filename];
  }
#endif

  // Count the progress of all tasks that are in the queue.
  // Files that need transcoding total 50 for the transcode and 50 for the copy, files that only need to be copied total 100.
  int progress = tasks_complete_ * 100;

  for (const Task &task : tasks_pending_) {
    progress += qBound(0, static_cast<int>(task.transcode_progress_ * 50), 50);
  }
#ifdef HAVE_GSTREAMER
  for (const Task &task : tasks_transcoding_.values()) {
    progress += qBound(0, static_cast<int>(task.transcode_progress_ * 50), 50);
  }
#endif

  // Add the progress of the track that's currently copying
  progress += current_copy_progress_;

  task_manager_->SetTaskProgress(task_id_, progress, total);

}

void Organize::FileTranscoded(const QString &input, const QString &output, bool success) {

  Q_UNUSED(output);

  qLog(Info) << "File finished" << input << success;
  transcode_progress_timer_.stop();

  Task task = tasks_transcoding_.take(input);
  if (!success) {
    files_with_errors_ << input;
  }
  else {
    tasks_pending_ << task;
  }
  QTimer::singleShot(0, this, SLOT(ProcessSomeFiles()));

}

void Organize::timerEvent(QTimerEvent *e) {

  QObject::timerEvent(e);

#ifdef HAVE_GSTREAMER
  if (e->timerId() == transcode_progress_timer_.timerId()) {
    UpdateProgress();
  }
#endif

}

void Organize::LogLine(const QString message) {

  QString date(QDateTime::currentDateTime().toString(Qt::TextDate));
  log_.append(QString("%1: %2").arg(date, message));

}
