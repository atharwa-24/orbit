// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "GpuTrack.h"

#include <limits>

#include "Capture.h"
#include "GlCanvas.h"
#include "Profiling.h"
#include "TimeGraph.h"
#include "absl/flags/flag.h"
#include "absl/strings/str_format.h"

using orbit_client_protos::TimerInfo;

constexpr const char* kSwQueueString = "sw queue";
constexpr const char* kHwQueueString = "hw queue";
constexpr const char* kHwExecutionString = "hw execution";

namespace OrbitGl {

std::string MapGpuTimelineToTrackLabel(std::string_view timeline) {
  std::string label;
  if (timeline.rfind("gfx", 0) == 0) {
    return absl::StrFormat("Graphics queue (%s)", timeline);
  } else if (timeline.rfind("sdma", 0) == 0) {
    return absl::StrFormat("Transfer queue (%s)", timeline);
  } else if (timeline.rfind("comp", 0) == 0) {
    return absl::StrFormat("Compute queue (%s)", timeline);
  } else {
    // On AMD, this should not happen and we don't support tracepoints for
    // other GPUs (at the moment). We return the timeline to make sure we
    // at least display something. When we add support for other GPU
    // tracepoints, this needs to be changed.
    return std::string(timeline);
  }
}

}  // namespace OrbitGl

//-----------------------------------------------------------------------------
GpuTrack::GpuTrack(TimeGraph* time_graph,
                   std::shared_ptr<StringManager> string_manager,
                   uint64_t timeline_hash)
    : Track(time_graph) {
  text_renderer_ = time_graph->GetTextRenderer();
  timeline_hash_ = timeline_hash;

  num_timers_ = 0;
  min_time_ = std::numeric_limits<TickType>::max();
  max_time_ = std::numeric_limits<TickType>::min();

  string_manager_ = string_manager;

  // Gpu tracks are collapsed by default.
  collapse_toggle_.SetState(
      TriangleToggle::State::kCollapsed,
      TriangleToggle::InitialStateUpdate::kReplaceInitialState);
}

//-----------------------------------------------------------------------------
void GpuTrack::Draw(GlCanvas* canvas, PickingMode picking_mode) {
  float track_height = GetHeight();
  float track_width = canvas->GetWorldWidth();

  SetPos(canvas->GetWorldTopLeftX(), m_Pos[1]);
  SetSize(track_width, track_height);

  Track::Draw(canvas, picking_mode);
}

//-----------------------------------------------------------------------------
Color GpuTrack::GetTimerColor(const TimerInfo& timer_info, bool is_selected,
                              bool inactive) const {
  const Color kInactiveColor(100, 100, 100, 255);
  const Color kSelectionColor(0, 128, 255, 255);
  if (is_selected) {
    return kSelectionColor;
  } else if (inactive) {
    return kInactiveColor;
  }

  // We color code the timeslices for GPU activity using the color
  // of the CPU thread track that submitted the job.
  Color color = time_graph_->GetThreadColor(timer_info.thread_id());

  // We disambiguate the different types of GPU activity based on the
  // string that is displayed on their timeslice.
  float coeff = 1.0f;
  std::string gpu_stage =
      string_manager_->Get(timer_info.user_data_key()).value_or("");
  if (gpu_stage == kSwQueueString) {
    coeff = 0.5f;
  } else if (gpu_stage == kHwQueueString) {
    coeff = 0.75f;
  } else if (gpu_stage == kHwExecutionString) {
    coeff = 1.0f;
  }

  color[0] = static_cast<uint8_t>(coeff * color[0]);
  color[1] = static_cast<uint8_t>(coeff * color[1]);
  color[2] = static_cast<uint8_t>(coeff * color[2]);

  constexpr uint8_t kOddAlpha = 210;
  if (!(timer_info.depth() & 0x1)) {
    color[3] = kOddAlpha;
  }

  return color;
}

//-----------------------------------------------------------------------------
inline float GetYFromDepth(const TimeGraphLayout& layout, float track_y,
                           uint32_t depth) {
  return track_y - layout.GetTextBoxHeight() * (depth + 1);
}

//-----------------------------------------------------------------------------
void GpuTrack::SetTimesliceText(const TimerInfo& timer_info, double elapsed_us,
                                float min_x, TextBox* text_box) {
  TimeGraphLayout layout = time_graph_->GetLayout();
  if (text_box->GetText().empty()) {
    std::string time = GetPrettyTime(absl::Microseconds(elapsed_us));

    text_box->SetElapsedTimeTextLength(time.length());

    CHECK(timer_info.type() == TimerInfo::kGpuActivity);

    std::string text = absl::StrFormat("%s  %s",
                                       time_graph_->GetStringManager()
                                           ->Get(timer_info.user_data_key())
                                           .value_or(""),
                                       time.c_str());
    text_box->SetText(text);
  }

  const Color kTextWhite(255, 255, 255, 255);
  const Vec2& box_pos = text_box->GetPos();
  const Vec2& box_size = text_box->GetSize();
  float pos_x = std::max(box_pos[0], min_x);
  float max_size = box_pos[0] + box_size[0] - pos_x;
  text_renderer_->AddTextTrailingCharsPrioritized(
      text_box->GetText().c_str(), pos_x,
      text_box->GetPosY() + layout.GetTextOffset(), GlCanvas::Z_VALUE_TEXT,
      kTextWhite, text_box->GetElapsedTimeTextLength(), max_size);
}

//-----------------------------------------------------------------------------
void GpuTrack::UpdatePrimitives(uint64_t min_tick, uint64_t max_tick,
                                PickingMode /*picking_mode*/) {
  Batcher* batcher = &time_graph_->GetBatcher();
  GlCanvas* canvas = time_graph_->GetCanvas();
  const TimeGraphLayout& layout = time_graph_->GetLayout();
  const TextBox& scene_box = canvas->GetSceneBox();

  float min_x = scene_box.GetPosX();
  float world_start_x = canvas->GetWorldTopLeftX();
  float world_width = canvas->GetWorldWidth();
  double inv_time_window = 1.0 / time_graph_->GetTimeWindowUs();
  bool is_collapsed = collapse_toggle_.IsCollapsed();

  std::vector<std::shared_ptr<TimerChain>> chains_by_depth = GetTimers();

  // We minimize overdraw when drawing lines for small events by discarding
  // events that would just draw over an already drawn line. When zoomed in
  // enough that all events are drawn as boxes, this has no effect. When zoomed
  // out, many events will be discarded quickly.
  uint64_t min_ignore = std::numeric_limits<uint64_t>::max();
  uint64_t max_ignore = std::numeric_limits<uint64_t>::min();

  uint64_t pixel_delta_in_ticks = static_cast<uint64_t>(MicrosecondsToTicks(
                                      time_graph_->GetTimeWindowUs())) /
                                  canvas->getWidth();
  uint64_t min_timegraph_tick =
      time_graph_->GetTickFromUs(time_graph_->GetMinTimeUs());

  for (auto& chain : chains_by_depth) {
    if (!chain) continue;
    for (TimerChainIterator it = chain->begin(); it != chain->end(); ++it) {
      TimerBlock& block = *it;
      if (!block.Intersects(min_tick, max_tick)) continue;
      // We have to reset this when we go to the next depth, as otherwise we
      // would miss drawing events that should be drawn.
      min_ignore = std::numeric_limits<uint64_t>::max();
      max_ignore = std::numeric_limits<uint64_t>::min();

      for (uint32_t k = 0; k < block.size(); ++k) {
        TextBox& text_box = block[k];
        const TimerInfo& timer_info = text_box.GetTimerInfo();
        if (min_tick > timer_info.end() || max_tick < timer_info.start())
          continue;
        if (timer_info.start() >= min_ignore && timer_info.end() <= max_ignore)
          continue;

        UpdateDepth(timer_info.depth() + 1);
        double start_us = time_graph_->GetUsFromTick(timer_info.start());
        double end_us = time_graph_->GetUsFromTick(timer_info.end());
        double elapsed_us = end_us - start_us;
        double normalized_start = start_us * inv_time_window;
        double normalized_length = elapsed_us * inv_time_window;
        float world_timer_width =
            static_cast<float>(normalized_length * world_width);
        float world_timer_x =
            static_cast<float>(world_start_x + normalized_start * world_width);
        uint8_t timer_depth = is_collapsed ? 0 : timer_info.depth();
        float world_timer_y = GetYFromDepth(layout, m_Pos[1], timer_depth);

        bool is_visible_width = normalized_length * canvas->getWidth() > 1;
        bool is_selected = &text_box == Capture::GSelectedTextBox;

        Vec2 pos(world_timer_x, world_timer_y);
        Vec2 size(world_timer_width, layout.GetTextBoxHeight());
        float z = GlCanvas::Z_VALUE_BOX_ACTIVE;
        Color color = GetTimerColor(timer_info, is_selected, false);
        text_box.SetPos(pos);
        text_box.SetSize(size);

        // When track is collapsed, only draw "hardware execution" timers.
        if (is_collapsed) {
          std::string gpu_stage =
              string_manager_->Get(timer_info.user_data_key()).value_or("");
          if (gpu_stage != kHwExecutionString) {
            continue;
          }
        }

        auto user_data = std::make_unique<PickingUserData>(
            &text_box, [&](PickingID id) { return this->GetBoxTooltip(id); });

        if (is_visible_width) {
          if (!is_collapsed) {
            SetTimesliceText(timer_info, elapsed_us, min_x, &text_box);
          }
          batcher->AddShadedBox(pos, size, z, color, PickingID::BOX,
                                std::move(user_data));
        } else {
          auto type = PickingID::LINE;
          batcher->AddVerticalLine(pos, size[1], z, color, type,
                                   std::move(user_data));
          // For lines, we can ignore the entire pixel into which this event
          // falls. We align this precisely on the pixel x-coordinate of the
          // current line being drawn (in ticks). If pixel_delta_in_ticks is
          // zero, we need to avoid dividing by zero, but we also wouldn't
          // gain anything here.
          if (pixel_delta_in_ticks != 0) {
            min_ignore = min_timegraph_tick +
                         ((timer_info.start() - min_timegraph_tick) /
                          pixel_delta_in_ticks) *
                             pixel_delta_in_ticks;
            max_ignore = min_ignore + pixel_delta_in_ticks;
          }
        }
      }
    }
  }
}

//-----------------------------------------------------------------------------
void GpuTrack::OnTimer(const TimerInfo& timer_info) {
  TextBox text_box(Vec2(0, 0), Vec2(0, 0), "", Color(255, 0, 0, 255));
  text_box.SetTimerInfo(timer_info);

  std::shared_ptr<TimerChain> timer_chain = timers_[timer_info.depth()];
  if (timer_chain == nullptr) {
    timer_chain = std::make_shared<TimerChain>();
    timers_[timer_info.depth()] = timer_chain;
  }
  timer_chain->push_back(text_box);
  ++num_timers_;
  if (timer_info.start() < min_time_) min_time_ = timer_info.start();
  if (timer_info.end() > max_time_) max_time_ = timer_info.end();
}

std::string GpuTrack::GetTooltip() const {
  return "Shows scheduling and execution times for selected GPU job "
         "submissions";
}

//-----------------------------------------------------------------------------
float GpuTrack::GetHeight() const {
  TimeGraphLayout& layout = time_graph_->GetLayout();
  bool collapsed = collapse_toggle_.IsCollapsed();
  uint32_t depth = collapsed ? 1 : GetDepth();
  return layout.GetTextBoxHeight() * depth + layout.GetTrackBottomMargin();
}

//-----------------------------------------------------------------------------
std::vector<std::shared_ptr<TimerChain>> GpuTrack::GetTimers() {
  std::vector<std::shared_ptr<TimerChain>> timers;
  ScopeLock lock(mutex_);
  for (auto& pair : timers_) {
    timers.push_back(pair.second);
  }
  return timers;
}

//-----------------------------------------------------------------------------
const TextBox* GpuTrack::GetFirstAfterTime(TickType time,
                                           uint32_t depth) const {
  std::shared_ptr<TimerChain> chain = GetTimers(depth);
  if (chain == nullptr) return nullptr;

  // TODO: do better than linear search...
  for (TimerChainIterator it = chain->begin(); it != chain->end(); ++it) {
    for (uint32_t k = 0; k < it->size(); ++k) {
      const TextBox& text_box = (*it)[k];
      if (text_box.GetTimerInfo().start() > time) {
        return &text_box;
      }
    }
  }

  return nullptr;
}

//-----------------------------------------------------------------------------
const TextBox* GpuTrack::GetFirstBeforeTime(TickType time,
                                            uint32_t depth) const {
  std::shared_ptr<TimerChain> chain = GetTimers(depth);
  if (chain == nullptr) return nullptr;

  const TextBox* text_box = nullptr;

  // TODO: do better than linear search...
  for (TimerChainIterator it = chain->begin(); it != chain->end(); ++it) {
    for (uint32_t k = 0; k < it->size(); ++k) {
      const TextBox& box = (*it)[k];
      if (box.GetTimerInfo().start() > time) {
        return text_box;
      }
      text_box = &box;
    }
  }

  return nullptr;
}

//-----------------------------------------------------------------------------
std::shared_ptr<TimerChain> GpuTrack::GetTimers(uint32_t depth) const {
  ScopeLock lock(mutex_);
  auto it = timers_.find(depth);
  if (it != timers_.end()) return it->second;
  return nullptr;
}

//-----------------------------------------------------------------------------
const TextBox* GpuTrack::GetLeft(TextBox* text_box) const {
  const TimerInfo& timer_info = text_box->GetTimerInfo();
  uint64_t timeline_hash = timer_info.user_data_key();
  if (timeline_hash == timeline_hash_) {
    std::shared_ptr<TimerChain> timers = GetTimers(timer_info.depth());
    if (timers) return timers->GetElementBefore(text_box);
  }
  return nullptr;
}

//-----------------------------------------------------------------------------
const TextBox* GpuTrack::GetRight(TextBox* text_box) const {
  const TimerInfo& timer_info = text_box->GetTimerInfo();
  uint64_t timeline_hash = timer_info.user_data_key();
  if (timeline_hash == timeline_hash_) {
    std::shared_ptr<TimerChain> timers = GetTimers(timer_info.depth());
    if (timers) return timers->GetElementAfter(text_box);
  }
  return nullptr;
}

//-----------------------------------------------------------------------------
const TextBox* GpuTrack::GetUp(TextBox* text_box) const {
  const TimerInfo& timer_info = text_box->GetTimerInfo();
  return GetFirstBeforeTime(timer_info.start(), timer_info.depth() - 1);
}

//-----------------------------------------------------------------------------
const TextBox* GpuTrack::GetDown(TextBox* text_box) const {
  const TimerInfo& timer_info = text_box->GetTimerInfo();
  return GetFirstAfterTime(timer_info.start(), timer_info.depth() + 1);
}

//-----------------------------------------------------------------------------
std::vector<std::shared_ptr<TimerChain>> GpuTrack::GetAllChains() {
  std::vector<std::shared_ptr<TimerChain>> chains;
  for (const auto& pair : timers_) {
    chains.push_back(pair.second);
  }
  return chains;
}

//-----------------------------------------------------------------------------
std::string GpuTrack::GetBoxTooltip(PickingID id) const {
  TextBox* text_box = time_graph_->GetBatcher().GetTextBox(id);
  if (!text_box ||
      text_box->GetTimerInfo().type() == TimerInfo::kCoreActivity) {
    return "";
  }

  std::string gpu_stage =
      string_manager_->Get(text_box->GetTimerInfo().user_data_key())
          .value_or("");
  if (gpu_stage == kSwQueueString) {
    return GetSwQueueTooltip(text_box->GetTimerInfo());
  } else if (gpu_stage == kHwQueueString) {
    return GetHwQueueTooltip(text_box->GetTimerInfo());
  } else if (gpu_stage == kHwExecutionString) {
    return GetHwExecutionTooltip(text_box->GetTimerInfo());
  }

  return "";
}

std::string GpuTrack::GetSwQueueTooltip(const TimerInfo& timer_info) const {
  return absl::StrFormat(
      "<b>Software Queue</b><br/>"
      "<i>Time between amdgpu_cs_ioctl (job submitted) and "
      "amdgpu_sched_run_job (job scheduled)</i>"
      "<br/>"
      "<br/>"
      "<b>Submitted from thread:</b> %s [%d]<br/>"
      "<b>Time:</b> %s",
      Capture::GThreadNames[timer_info.thread_id()].c_str(),
      timer_info.thread_id(),
      GetPrettyTime(TicksToDuration(timer_info.start(), timer_info.end()))
          .c_str());
}

std::string GpuTrack::GetHwQueueTooltip(const TimerInfo& timer_info) const {
  return absl::StrFormat(
      "<b>Hardware Queue</b><br/><i>Time between amdgpu_sched_run_job "
      "(job scheduled) and start of GPU execution</i>"
      "<br/>"
      "<br/>"
      "<b>Submitted from thread:</b> %s [%d]<br/>"
      "<b>Time:</b> %s",
      Capture::GThreadNames[timer_info.thread_id()].c_str(),
      timer_info.thread_id(),
      GetPrettyTime(TicksToDuration(timer_info.start(), timer_info.end()))
          .c_str());
}

std::string GpuTrack::GetHwExecutionTooltip(const TimerInfo& timer_info) const {
  return absl::StrFormat(
      "<b>Harware Execution</b><br/>"
      "<i>End is marked by \"dma_fence_signaled\" event for this command "
      "buffer submission</i>"
      "<br/>"
      "<br/>"
      "<b>Submitted from thread:</b> %s [%d]<br/>"
      "<b>Time:</b> %s",
      Capture::GThreadNames[timer_info.thread_id()].c_str(),
      timer_info.thread_id(),
      GetPrettyTime(TicksToDuration(timer_info.start(), timer_info.end()))
          .c_str());
}
