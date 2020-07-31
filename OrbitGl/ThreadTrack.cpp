// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ThreadTrack.h"

#include <limits>

#include "Capture.h"
#include "EventTrack.h"
#include "FunctionUtils.h"
#include "GlCanvas.h"
#include "Profiling.h"
#include "TextBox.h"
#include "TimeGraph.h"
#include "absl/flags/flag.h"
#include "absl/strings/str_format.h"

// TODO: Remove this flag once we have a way to toggle the display return values
ABSL_FLAG(bool, show_return_values, false, "Show return values on time slices");

using orbit_client_protos::FunctionInfo;
using orbit_client_protos::TimerInfo;

//-----------------------------------------------------------------------------
ThreadTrack::ThreadTrack(TimeGraph* time_graph, int32_t thread_id)
    : Track(time_graph) {
  text_renderer_ = time_graph->GetTextRenderer();
  thread_id_ = thread_id;

  num_timers_ = 0;
  min_time_ = std::numeric_limits<TickType>::max();
  max_time_ = std::numeric_limits<TickType>::min();

  event_track_ = std::make_shared<EventTrack>(time_graph);
  event_track_->SetThreadId(thread_id);
}

//-----------------------------------------------------------------------------
void ThreadTrack::Draw(GlCanvas* canvas, PickingMode picking_mode) {
  float track_height = GetHeight();
  float track_width = canvas->GetWorldWidth();

  SetPos(canvas->GetWorldTopLeftX(), m_Pos[1]);
  SetSize(track_width, track_height);

  Track::Draw(canvas, picking_mode);

  // Event track
  if (HasEventTrack()) {
    float event_track_height = time_graph_->GetLayout().GetEventTrackHeight();
    event_track_->SetPos(m_Pos[0], m_Pos[1]);
    event_track_->SetSize(track_width, event_track_height);
    event_track_->Draw(canvas, picking_mode);
  }
}

//-----------------------------------------------------------------------------
std::string ThreadTrack::GetExtraInfo(const TimerInfo& timer_info) {
  std::string info;
  static bool show_return_value = absl::GetFlag(FLAGS_show_return_values);
  if (show_return_value && timer_info.type() == TimerInfo::kNone) {
    info = absl::StrFormat("[%lu]", timer_info.user_data_key());
  }
  return info;
}

//-----------------------------------------------------------------------------
inline Color GetTimerColor(const TimerInfo& timer_info, TimeGraph* time_graph,
                           bool is_selected, bool inactive) {
  const Color kInactiveColor(100, 100, 100, 255);
  const Color kSelectionColor(0, 128, 255, 255);
  if (is_selected) {
    return kSelectionColor;
  } else if (inactive) {
    return kInactiveColor;
  }

  Color color = time_graph->GetThreadColor(timer_info.thread_id());

  constexpr uint8_t kOddAlpha = 210;
  if (!(timer_info.depth() & 0x1)) {
    color[3] = kOddAlpha;
  }

  return color;
}

//-----------------------------------------------------------------------------
float ThreadTrack::GetYFromDepth(float track_y, uint32_t depth,
                                 bool collapsed) {
  const TimeGraphLayout& layout = time_graph_->GetLayout();
  float box_height = layout.GetTextBoxHeight();
  if (collapsed && depth_ > 0) {
    box_height /= static_cast<float>(depth_);
  }

  return track_y - layout.GetEventTrackHeight() -
         layout.GetSpaceBetweenTracksAndThread() - box_height * (depth + 1);
}

float ThreadTrack::GetYFromDepth(uint32_t depth) {
  return GetYFromDepth(m_Pos[1], depth, collapse_toggle_.IsCollapsed());
}

//-----------------------------------------------------------------------------
void ThreadTrack::SetTimesliceText(const TimerInfo& timer_info,
                                   double elapsed_us, float min_x,
                                   TextBox* text_box) {
  TimeGraphLayout layout = time_graph_->GetLayout();
  if (text_box->GetText().empty()) {
    std::string time = GetPrettyTime(absl::Microseconds(elapsed_us));
    FunctionInfo* func =
        Capture::GSelectedFunctionsMap[timer_info.function_address()];

    text_box->SetElapsedTimeTextLength(time.length());

    const char* name = nullptr;
    if (func) {
      std::string extra_info = GetExtraInfo(timer_info);
      name = FunctionUtils::GetDisplayName(*func).c_str();
      std::string text =
          absl::StrFormat("%s %s %s", name, extra_info.c_str(), time.c_str());

      text_box->SetText(text);
    } else if (timer_info.type() == TimerInfo::kIntrospection) {
      std::string text = absl::StrFormat("%s %s",
                                         time_graph_->GetStringManager()
                                             ->Get(timer_info.user_data_key())
                                             .value_or(""),
                                         time.c_str());
      text_box->SetText(text);
    } else {
      ERROR("Unexpected case in ThreadTrack::SetTimesliceText");
      PRINT_VAR(timer_info.type());
      PRINT_VAR(func);
    }
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
void ThreadTrack::UpdatePrimitives(uint64_t min_tick, uint64_t max_tick, PickingMode picking_mode) {
  event_track_->SetPos(m_Pos[0], m_Pos[1]);
  event_track_->UpdatePrimitives(min_tick, max_tick, picking_mode);

  Batcher* batcher = &time_graph_->GetBatcher();
  GlCanvas* canvas = time_graph_->GetCanvas();
  const TimeGraphLayout& layout = time_graph_->GetLayout();
  const TextBox& scene_box = canvas->GetSceneBox();

  float min_x = scene_box.GetPosX();
  float world_start_x = canvas->GetWorldTopLeftX();
  float world_width = canvas->GetWorldWidth();
  double inv_time_window = 1.0 / time_graph_->GetTimeWindowUs();
  bool is_collapsed = collapse_toggle_.IsCollapsed();
  float box_height = layout.GetTextBoxHeight();
  if (is_collapsed && depth_ > 0) {
    box_height /= static_cast<float>(depth_);
  }

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

      for (size_t k = 0; k < block.size(); ++k) {
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
        float world_timer_y = GetYFromDepth(timer_info.depth());

        bool is_visible_width = normalized_length * canvas->getWidth() > 1;
        bool is_selected = &text_box == Capture::GSelectedTextBox;
        bool is_inactive =
            !Capture::GVisibleFunctionsMap.empty() &&
            Capture::GVisibleFunctionsMap[timer_info.function_address()] ==
                nullptr;

        Vec2 pos(world_timer_x, world_timer_y);
        Vec2 size(world_timer_width, box_height);
        float z = GlCanvas::Z_VALUE_BOX_ACTIVE;
        Color color =
            GetTimerColor(timer_info, time_graph_, is_selected, is_inactive);
        text_box.SetPos(pos);
        text_box.SetSize(size);

        auto user_data = std::make_unique<PickingUserData>(
          &text_box, [&](PickingID id) { return this->GetBoxTooltip(id); });

        if (is_visible_width) {
          if (!is_collapsed) {
            SetTimesliceText(timer_info, elapsed_us, min_x, &text_box);
          }
          batcher->AddShadedBox(pos, size, z, color, PickingID::BOX, std::move(user_data));
        } else {
          auto type = PickingID::LINE;
          batcher->AddVerticalLine(pos, size[1], z, color, type, std::move(user_data));
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
void ThreadTrack::OnTimer(const TimerInfo& timer_info) {
  if (timer_info.type() != TimerInfo::kCoreActivity) {
    UpdateDepth(timer_info.depth() + 1);
  }

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

std::string ThreadTrack::GetTooltip() const {
  return "Shows collected samples and timings from dynamically instrumented functions";
}

//-----------------------------------------------------------------------------
float ThreadTrack::GetHeight() const {
  TimeGraphLayout& layout = time_graph_->GetLayout();
  bool is_collapsed = collapse_toggle_.IsCollapsed();
  uint32_t collapsed_depth = (GetNumTimers() == 0) ? 0 : 1;
  uint32_t depth = is_collapsed ? collapsed_depth : GetDepth();
  return layout.GetTextBoxHeight() * depth +
         (depth > 0 ? layout.GetSpaceBetweenTracksAndThread() : 0) +
         layout.GetEventTrackHeight() + layout.GetTrackBottomMargin();
}

//-----------------------------------------------------------------------------
std::vector<std::shared_ptr<TimerChain>> ThreadTrack::GetTimers() {
  std::vector<std::shared_ptr<TimerChain>> timers;
  ScopeLock lock(mutex_);
  for (auto& pair : timers_) {
    timers.push_back(pair.second);
  }
  return timers;
}

//-----------------------------------------------------------------------------
const TextBox* ThreadTrack::GetFirstAfterTime(TickType time,
                                              uint32_t depth) const {
  std::shared_ptr<TimerChain> chain = GetTimers(depth);
  if (chain == nullptr) return nullptr;

  // TODO: do better than linear search...
  for (TimerChainIterator it = chain->begin(); it != chain->end(); ++it) {
    for (size_t k = 0; k < it->size(); ++k) {
      const TextBox& text_box = (*it)[k];
      if (text_box.GetTimerInfo().start() > time) {
        return &text_box;
      }
    }
  }
  return nullptr;
}

//-----------------------------------------------------------------------------
const TextBox* ThreadTrack::GetFirstBeforeTime(TickType time,
                                               uint32_t depth) const {
  std::shared_ptr<TimerChain> chain = GetTimers(depth);
  if (chain == nullptr) return nullptr;

  const TextBox* text_box = nullptr;

  // TODO: do better than linear search...
  for (TimerChainIterator it = chain->begin(); it != chain->end(); ++it) {
    for (size_t k = 0; k < it->size(); ++k) {
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
std::shared_ptr<TimerChain> ThreadTrack::GetTimers(uint32_t depth) const {
  ScopeLock lock(mutex_);
  auto it = timers_.find(depth);
  if (it != timers_.end()) return it->second;
  return nullptr;
}

//-----------------------------------------------------------------------------
const TextBox* ThreadTrack::GetLeft(TextBox* text_box) const {
  const TimerInfo& timer_info = text_box->GetTimerInfo();
  if (timer_info.thread_id() == thread_id_) {
    std::shared_ptr<TimerChain> timers = GetTimers(timer_info.depth());
    if (timers) return timers->GetElementBefore(text_box);
  }
  return nullptr;
}

//-----------------------------------------------------------------------------
const TextBox* ThreadTrack::GetRight(TextBox* text_box) const {
  const TimerInfo& timer_info = text_box->GetTimerInfo();
  if (timer_info.thread_id() == thread_id_) {
    std::shared_ptr<TimerChain> timers = GetTimers(timer_info.depth());
    if (timers) return timers->GetElementAfter(text_box);
  }
  return nullptr;
}

//-----------------------------------------------------------------------------
const TextBox* ThreadTrack::GetUp(TextBox* text_box) const {
  const TimerInfo& timer_info = text_box->GetTimerInfo();
  return GetFirstBeforeTime(timer_info.start(), timer_info.depth() - 1);
}

//-----------------------------------------------------------------------------
const TextBox* ThreadTrack::GetDown(TextBox* text_box) const {
  const TimerInfo& timer_info = text_box->GetTimerInfo();
  return GetFirstAfterTime(timer_info.start(), timer_info.depth() + 1);
}

//-----------------------------------------------------------------------------
std::vector<std::shared_ptr<TimerChain>> ThreadTrack::GetAllChains() {
  std::vector<std::shared_ptr<TimerChain>> chains;
  for (const auto& pair : timers_) {
    chains.push_back(pair.second);
  }
  return chains;
}

//-----------------------------------------------------------------------------
void ThreadTrack::SetEventTrackColor(Color color) {
  ScopeLock lock(mutex_);
  event_track_->SetColor(color);
}

//-----------------------------------------------------------------------------
bool ThreadTrack::IsEmpty() const {
  return (GetNumTimers() == 0) && event_track_->IsEmpty();
}

//-----------------------------------------------------------------------------
std::string ThreadTrack::GetBoxTooltip(PickingID id) const {
  TextBox* text_box = time_graph_->GetBatcher().GetTextBox(id);
  if (!text_box ||
      text_box->GetTimerInfo().type() == TimerInfo::kCoreActivity) {
    return "";
  }

  FunctionInfo* func = Capture::GSelectedFunctionsMap[text_box->GetTimerInfo()
                                                          .function_address()];
  if (!func) {
    return text_box->GetText();
  }

  return absl::StrFormat(
    "<b>%s</b><br/>"
    "<i>Timing measured through dynamic instrumentation</i>"
    "<br/><br/>"
    "<b>Module:</b> %s<br/>"
    "<b>Time:</b> %s",
    FunctionUtils::GetDisplayName(*func),
    FunctionUtils::GetLoadedModuleName(*func),
    GetPrettyTime(TicksToDuration(text_box->GetTimerInfo().start(),
                                  text_box->GetTimerInfo().end()))
  );
}
