#define NOMINMAX
#include "base/http.h"
#include "data.h"
#include "base/thread.h"
#include "app.h"
#include "wizard.h"
#include <algorithm>
#include <time.h>

void ProgramData::Task::stop() {
  terminate_ = true;
  if (thread_) {
    if (WaitForSingleObject(thread_, INFINITE) != WAIT_OBJECT_0) {
      // don't do this, it will likely crash everything
      TerminateThread(thread_, 0);
    }
    CloseHandle(thread_);
    thread_ = nullptr;
  }
}
ProgramData::Task::~Task() {
  stop();
}

void ProgramData::Task::start(ProgramData* data) {
  data_ = data;
  callback_ = data->app_->wizard()->page();
  thread_ = thread::create(this, &Task::proc);
}

void ProgramData::Task::notify(uint32 value) {
  if (!terminate_ && callback_) {
    uint32 time = GetTickCount();
    if (time > lastNotify_ + 500 || value != lastNotifyCode_) {
      PostMessage(data_->app_->wizard()->getHandle(), WM_TASKDONE, (WPARAM)callback_, value);
      lastNotify_ = time;
      lastNotifyCode_ = value;
      if (lastNotifyCode_ >= LOADING) lastNotifyCode_ = 0;
    }
  }
}
int ProgramData::Task::proc() {
  try {
    uint32 res = run();
    notify(res);
  } catch (Exception& ex) {
    notify(-1);
  }
  return 0;
}

void ProgramData::setTask(Task* task) {
  // duplicating the unique_ptr just in case
  {
    auto tsk = task_;
    if (tsk) tsk->stop();
  }
  task_.reset(task);
  if (task) task->start(this);
}

////////////////////////////

void ProgramData::setProgram(std::string const& program) {
  class ProgramTask : public Task {
  public:
    ProgramTask(std::string const& program)
      : program_(program)
    {}
  private:
    std::string program_;
    uint32 run() {
      NGDP::NGDP* ngdp = new NGDP::NGDP(program_);
      if (ngdp && ngdp->regions().empty()) {
        delete ngdp;
        ngdp = ngdp;
      }
      if (!terminate_) {
        data_->ngdp_.reset(ngdp);
      }
      return 0;
    }
  };

  if (!program.empty()) {
    setTask(new ProgramTask(program));
  } else {
    setTask(nullptr);
    ngdp_.reset();
  }
}

//////////////////////////////////

void ProgramData::loadBuilds() {
  class BuildTask : public Task {
  private:
    uint32 run() {
      if (!data_->ngdp_->version()) return -1;
      std::string cdn_hash = data_->ngdp_->version()->cdn;
      File file = data_->ngdp_->load(cdn_hash);
      if (!file) return -1;
      data_->cdn_config = NGDP::ParseConfig(file);
	  data_->builds.push_back(data_->ngdp_->version()->build);
      notify(0);
      for (size_t i = 0; i < data_->builds.size() && !terminate_; ++i) {
        std::string build = data_->builds[i];
        File file = data_->ngdp_->load(build);
        if (!file) continue;
        data_->build_configs[build] = NGDP::ParseConfig(file);
        notify(i + 1);
      }
      if (!terminate_) {
        data_->builds_loaded = cdn_hash;
      }
      return data_->builds.size();
    }
  };

  builds_loaded.clear();
  setTask(new BuildTask);
}

/////////////////////////////////////

File ProgramData::Task::loadData(std::string const& hash, int idx) {
  File file = NGDP::CascStorage::getCache(hash);
  if (file) return file;
  file = data_->ngdp_->load(hash, "data");
  if (!file) return file;

  data_->loading_size = file.size();
  if (data_->loading_size >= (1 << 20)) {
    static uint8 buffer[1 << 18];
    while (file.tell() < data_->loading_size && !terminate_) {
      file.read(buffer, sizeof buffer);
      data_->loading_progress = file.tell();
      notify(LOADING + idx);
    }
  }

  if (file) NGDP::CascStorage::addCache(hash, file);
  return file;
}

std::vector<ProgramData::Tag> ProgramData::loadTags(File& file, uint32 numTags, uint32 numEntries) {
  std::vector<Tag> result;
  uint32 maskSize = (numEntries + 7) / 8;
  while (numTags--) {
    result.emplace_back();
    auto& tag = result.back();
    while (char chr = file.getc()) {
      tag.name.push_back(chr);
    }
    tag.type = file.read16(true);
    tag.mask.resize(maskSize);
    file.read(&tag.mask[0], maskSize);
  }
  return result;
}

void ProgramData::loadTags() {
  class TagTask : public Task {
  private:
    uint32 run() {
      // get build config
      if (!data_->build_configs.count(data_->selected_build)) return -1;
      auto buildConfig = data_->build_configs[data_->selected_build];

      // load encoding file
      File file = loadData(split(buildConfig["encoding"])[1], 0);
      if (!file || terminate_) return -1;
      data_->encoding_.reset(new NGDP::Encoding(NGDP::DecodeBLTE(file, std::stoi(split(buildConfig["encoding-size"])[0]))));

      // get download CDN hash
      NGDP::Hash hash;
      NGDP::from_string(hash, buildConfig["download"]);
      auto const* enc = data_->encoding_->getEncoding(hash);
      if (!enc) return -1;

      // load download file
      file = loadData(NGDP::to_string(enc->keys[0]), 1);
      if (!file || terminate_) return -1;
      file = NGDP::DecodeBLTE(file, enc->usize);

      // parse download file
      if (file.read16(true) != 'DL') return -1;
      file.seek(3, SEEK_CUR);
      uint32 numEntries = file.read32(true);
      uint16 numTags = file.read16(true);
      data_->file_sizes_.clear();
      for (uint32 i = 0; i < numEntries; ++i) {
        file.read(hash, sizeof hash);
        file.seek(10, SEEK_CUR);
        auto const* layout = data_->encoding_->getLayout(hash);
        data_->file_sizes_.push_back(layout ? layout->csize : 0);
      }
      data_->tags_ = ProgramData::loadTags(file, numTags, numEntries);
      std::sort(data_->tags_.begin(), data_->tags_.end(), [](Tag const& lhs, Tag const& rhs) {
        return lhs.type < rhs.type;
      });
      std::vector<std::vector<std::string>> tags;
      for (size_t i = 0; i < data_->tags_.size(); ++i) {
        if (i == 0 || data_->tags_[i].type != data_->tags_[i - 1].type) {
          tags.emplace_back();
        }
        tags.back().push_back(data_->tags_[i].name);
      }
      data_->tags = tags;
      return 0;
    }
  };

  tags.clear();
  setTask(new TagTask);
}

std::vector<uint8> ProgramData::downloadMask() {
  size_t maskSize = (file_sizes_.size() + 7) / 8;
  std::vector<uint8> mask(maskSize, 0);
  for (std::string const& tags : used_tags) {
    std::vector<uint8> cur_mask(maskSize, 0xFF);
    for (std::string const& tag : split(tags)) {
      for (size_t i = 0; i < tags_.size(); ++i) {
        if (tags_[i].name == tag) {
          for (size_t j = 0; j < maskSize; ++j) {
            cur_mask[j] &= tags_[i].mask[j];
          }
          break;
        }
      }
    }
    for (size_t j = 0; j < maskSize; ++j) {
      mask[j] |= cur_mask[j];
    }
  }
  return mask;
}
uint64 ProgramData::downloadSize() {
  auto mask = downloadMask();
  uint64 size = 0;
  for (size_t i = 0; i < file_sizes_.size(); ++i) {
    if (mask[i / 8] & (1 << (7 - (i & 7)))) {
      size += file_sizes_[i];
    }
  }
  return size;
}

void ProgramData::download(std::string const& path) {
  class DownloadTask : public Task {
  public:
    DownloadTask(std::string const& path)
      : path_(path)
    {}
  private:
    std::string path_;
    uint32 run() {
      auto& progress = data_->progress_;
      progress.reset(new Progress);
      auto& ngdp = data_->ngdp_;

      try {
        NGDP::CascStorage storage(path_ / "Data");
        NGDP::DataStorage data(storage);
        NGDP::Hash hash;

        // save configs

        storage.addConfig(ngdp->version()->cdn, ngdp->load(ngdp->version()->cdn));
        storage.addConfig(data_->selected_build, ngdp->load(data_->selected_build));

        // save encoding file

        auto buildConfig = data_->build_configs[data_->selected_build];
        std::string encodingHash = split(buildConfig["encoding"])[1];
        NGDP::from_string(hash, encodingHash);
        data.addFile(hash, loadData(encodingHash));

        auto& encoding = data_->encoding_;

        // write .build.info

        File info(path_ / ".build.info", File::REWRITE);
        info.printf("Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|CDN Key!HEX:16");
        info.printf("|Install Key!HEX:16|IM Size!DEC:4|CDN Path!STRING:0");
        info.printf("|CDN Hosts!STRING:0|Tags!STRING:0|Armadillo!STRING:0");
        info.printf("|Last Activated!STRING:0|Version!STRING:0|Keyring!HEX:16|KeyService!STRING:0\n");

        NGDP::from_string(hash, buildConfig["install"]);
        auto const* enc = encoding->getEncoding(hash);
        auto const* cdnData = ngdp->cdn();
        info.printf("%s|1|%s|%s", ngdp->region().c_str(), data_->selected_build.c_str(), ngdp->version()->cdn.c_str());
        info.printf("|%s|%u|%s", NGDP::to_string(enc->keys[0]).c_str(), enc->usize, cdnData ? cdnData->path.c_str() : "");
        info.printf("|%s|%s|", cdnData ? join(cdnData->hosts, " ").c_str() : "", join(data_->used_tags, ":").c_str());

        __time64_t curtime;
        tm timestruct;
        char timebuf[128];
        _time64(&curtime);
        _gmtime64_s(&timestruct, &curtime);
        strftime(timebuf, sizeof timebuf, "%Y-%m-%dT%H:%M:%SZ", &timestruct);
        std::vector<std::string> version;
        std::string build_id;
        for (auto const& p : split_multiple(buildConfig["build-name"], "_- ")) {
          if (!p.empty() && std::isdigit((unsigned char) p[0])) {
            std::string d;
            for (size_t i = 0; i < p.size() && std::isdigit((unsigned char)p[i]); ++i) {
              d.push_back(p[i]);
            }
            if (d.size() >= 4) {
              build_id = d;
            } else {
              version.push_back(d);
            }
          }
        }
        info.printf("|%s|%s||\n", timebuf, (join(version, ".") + "." + build_id).c_str());

        // load indices

        loadIndices(storage);

        log("Downloading files...");

        // fetch download file

        NGDP::from_string(hash, buildConfig["download"]);
        enc = encoding->getEncoding(hash);
        File download = NGDP::DecodeBLTE(data.addFile(enc->keys[0], loadData(enc->keys[0])));
        if (download.read16(true) != 'DL') {
          throw Exception("invalid download file");
        }
        if (terminate_) return 1;
        download.seek(3, SEEK_CUR);
        uint32 numEntries = download.read32(true);
        uint32 numTags = download.read16(true);
        uint32 filesPos = download.tell();
        download.seek(26 * numEntries, SEEK_CUR);
        data_->tags_ = ProgramData::loadTags(download, numTags, numEntries);
        auto mask = data_->downloadMask();

        // calculate size

        progress->filesTotal = 0;
        progress->filesDone = 0;
        download.seek(filesPos, SEEK_SET);
        for (uint32 i = 0; i < numEntries; ++i) {
          download.read(hash, sizeof hash);
          download.seek(10, SEEK_CUR);
          if (!(mask[i / 8] & (1 << (7 - (i & 7))))) continue;
          ++progress->filesTotal;
          auto const* layout = encoding->getLayout(hash);
          if (layout) progress->sizeTotal += layout->csize;
        }
        notify(0);

        // download

        uint32 failedFiles = 0;
        download.seek(filesPos, SEEK_SET);
        for (uint32 i = 0; i < numEntries && !terminate_; ++i) {
          download.read(hash, sizeof hash);
          download.seek(10, SEEK_CUR);
          if (!(mask[i / 8] & (1 << (7 - (i & 7))))) continue;

          File file = loadFile(hash);
          if (file) {
            data.addFile(hash, file);
          } else {
            ++failedFiles;
          }

          auto const* layout = encoding->getLayout(hash);
          if (layout) progress->sizeDone += layout->csize;
          ++progress->filesDone;
          notify(0);
        }
        if (terminate_) return 1;

        if (failedFiles) {
          log(fmtstring("Failed to download %u files", failedFiles));
        }

        log("Installing files...");

        // fetch install file

        NGDP::from_string(hash, buildConfig["install"]);
        enc = encoding->getEncoding(hash);
        File install = NGDP::DecodeBLTE(data.addFile(enc->keys[0], loadData(enc->keys[0])));
        if (install.read16(true) != 'IN') {
          throw Exception("invalid install file");
        }
        if (terminate_) return 1;
        install.seek(2, SEEK_CUR);
        numTags = install.read16(true);
        numEntries = install.read32(true);
        data_->tags_ = ProgramData::loadTags(install, numTags, numEntries);
        mask = data_->downloadMask();

        progress->filesTotal = 0;
        progress->filesDone = 0;
        progress->sizeTotal = 0;
        progress->sizeDone = 0;
        // calculate size
        uint32 installPos = install.tell();
        for (uint32 i = 0; i < numEntries; ++i) {
          std::string name;
          while (char c = install.getc()) {
            name.push_back(c);
          }
          install.read(hash, sizeof hash);
          uint32 usize = install.read32(true);
          if (!(mask[i / 8] & (1 << (7 - (i & 7))))) continue;
          ++progress->filesTotal;
          progress->sizeTotal += usize;
        }
        notify(0);

        // install

        install.seek(installPos, SEEK_SET);
        for (uint32 i = 0; i < numEntries && !terminate_; ++i) {
          std::string name;
          while (char c = install.getc()) {
            name.push_back(c);
          }
          install.read(hash, sizeof hash);
          uint32 usize = install.read32(true);
          if (!(mask[i / 8] & (1 << (7 - (i & 7))))) continue;

          enc = encoding->getEncoding(hash);
          if (!enc) {
            log("Failed to extract " + name);
            continue;
          }
          File file = loadFile(enc->keys[0]);
          if (!file) {
            log("Failed to extract " + name);
            continue;
          }
          File(path_ / name, File::REWRITE).copy(NGDP::DecodeBLTE(file, usize));

          ++progress->filesDone;
          progress->sizeDone += usize;
          notify(0);
        }
        log("Done");
      } catch (Exception& ex) {
        log(fmtstring("Error: %s", ex.what()));
      }

      return 1;
    }

    void log(std::string const& text) {
      data_->progress_->log.push_back(text);
      notify(0);
    }

    struct IndexEntry {
      uint16 index;
      uint32 size;
      uint32 offset;
    };
    struct ArchiveInfo {
      std::string name;
      std::vector<uint8> mask;
    };
    enum { BlockSize = (1 << 20) };
    std::vector<ArchiveInfo> archives_;
    std::unordered_map<NGDP::Hash_container, IndexEntry, NGDP::Hash_container::hash, NGDP::Hash_container::equal> index_;

    void loadIndices(NGDP::CascStorage& storage) {
      NGDP::Hash nilHash;
      memset(nilHash, 0, sizeof nilHash);

      std::vector<std::string> archives = split(data_->cdn_config["archives"]);
      auto& progress = data_->progress_;
      progress->filesTotal = archives.size();
      log("Loading indices...");

      archives_.resize(archives.size());
      for (size_t i = 0; i < archives.size() && !terminate_; ++i) {
        archives_[i].name = archives[i];

        File index = NGDP::CascStorage::getCache(archives[i] + ".index");
        if (!index) {
          File src = data_->ngdp_->load(archives[i], "data", true);
          if (!src) continue;
          index = NGDP::CascStorage::addCache(archives[i] + ".index", src);
          storage.addIndex(archives[i], src);
        }
        progress->filesDone = i;
        notify(0);

        size_t size = index.size();
        for (size_t block = 0; block + 4096 <= size; block += 4096) {
          NGDP::Hash hash;
          index.seek(block, SEEK_SET);
          for (size_t pos = 0; pos + 24 <= 4096; pos += 24) {
            index.read(hash, sizeof hash);
            if (!memcmp(hash, nilHash, sizeof hash)) {
              block = size;
              break;
            }
            auto& dst = index_[NGDP::Hash_container::from(hash)];
            dst.index = i;
            dst.size = index.read32(true);
            dst.offset = index.read32(true);
          }
        }

        File mask = NGDP::CascStorage::getCache(archives[i] + ".mask");
        if (!mask) {
          File raw = NGDP::CascStorage::getCache(archives[i]);
          if (raw) {
            uint32 chunks = (raw.size() + BlockSize - 1) / BlockSize;
            archives_[i].mask.assign((chunks + 7) / 8, 0xFF);
            continue;
          }
        } else {
          archives_[i].mask.resize(mask.size());
          mask.read(&archives_[i].mask[0], archives_[i].mask.size());
        }
      }
    }

    File loadFile(NGDP::Hash const& hash) {
      auto it = index_.find(NGDP::Hash_container::from(hash));
      if (it == index_.end()) return loadData(hash);

      uint32 offset = it->second.offset;
      uint32 size = it->second.size;
      uint32 blockStart = offset / BlockSize;
      uint32 blockEnd = (offset + size + BlockSize - 1) / BlockSize;
      auto& mask = archives_[it->second.index].mask;
      uint32 getStart = blockEnd;
      uint32 getEnd = blockStart;
      for (uint32 i = blockStart; i < blockEnd; ++i) {
        if (i / 8 >= mask.size() || !(mask[i / 8] & (1 << (i & 7)))) {
          getStart = std::min(getStart, i);
          getEnd = std::max(getEnd, i + 1);
        }
      }
      if (getStart < getEnd) {
        std::string url = data_->ngdp_->geturl(archives_[it->second.index].name, "data");
        HttpRequest request(url);
        request.addHeader(fmtstring("Range: bytes=%u-%u", getStart * BlockSize, getEnd * BlockSize - 1));
        request.send();
        uint32 status = request.status();
        if (status != 200 && status != 206) return File();
        auto headers = request.headers();
        File result = request.response();
        File archive = NGDP::CascStorage::addCache(archives_[it->second.index].name);
        if (headers.count("Content-Range")) {
          uint32 start, end, total;
          if (sscanf(headers["Content-Range"].c_str(), "bytes %u-%u/%u", &start, &end, &total) != 3) {
            return File();
          }
          if (archive.size() < total) {
            archive.seek(total - 1, SEEK_SET);
            archive.putc(0);
          }
          archive.seek(start, SEEK_SET);
          archive.copy(result, end - start + 1);
          start = (start + BlockSize - 1) / BlockSize;
          if (end >= total - 1) {
            end = (end + BlockSize) / BlockSize;
          } else {
            end = (end + 1) / BlockSize;
          }
          total = (total + BlockSize - 1) / BlockSize;
          mask.resize((total + 7) / 8, 0);
          for (uint32 i = start; i < end; ++i) {
            mask[i / 8] |= (1 << (i & 7));
          }
          File file = NGDP::CascStorage::addCache(archives_[it->second.index].name + ".mask");
          file.write(&mask[0], mask.size());
        } else {
          archive.copy(result);
        }
      }

      File data = NGDP::CascStorage::getCache(archives_[it->second.index].name);
      if (!data) return File();
      MemoryFile result;
      data.seek(offset, SEEK_SET);
      data.read(result.reserve(size), size);
      result.seek(0);
      return result;
    }
  };

  setTask(new DownloadTask(path));
}
