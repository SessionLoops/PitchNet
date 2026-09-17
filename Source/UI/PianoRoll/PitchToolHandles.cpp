#include "PitchToolHandles.h"
#include "BinaryData.h"

#include <algorithm>
#include <limits>

namespace {

juce::Image loadIcon(const char* resourceName)
{
  int size = 0;
  const auto* data = BinaryData::getNamedResource(resourceName, size);
  return data ? juce::ImageFileFormat::loadFrom(data, size) : juce::Image();
}

} // namespace

PitchToolHandles::PitchToolHandles()
    : leftTiltIcon(loadIcon("ltilt_png")),
      vibratoIcon(loadIcon("vibrato_png")),
      driftIcon(loadIcon("drift_png")),
      formantIcon(loadIcon("formant_png")),
      amplitudeIcon(loadIcon("amplitude_png")),
      rightTiltIcon(loadIcon("rtilt_png")) {
  // Initialize (currently empty, but reserve space)
  handles.reserve(20);  // Typical max handles for multi-note selection
}

void PitchToolHandles::updateHandles(const std::vector<Note*>& selectedNotes,
                                     const CoordinateMapper& mapper,
                                     const juce::Rectangle<float>& hoverBounds) {
  handles.clear();

  if (selectedNotes.empty())
    return;

  int minStartFrame = std::numeric_limits<int>::max();
  int maxEndFrame = std::numeric_limits<int>::min();
  float maxMidi = std::numeric_limits<float>::lowest();

  for (const auto* note : selectedNotes) {
    if (!note)
      continue;

    minStartFrame = std::min(minStartFrame, note->getStartFrame());
    maxEndFrame = std::max(maxEndFrame, note->getEndFrame());
    maxMidi = std::max(maxMidi, note->getMidiNote());
  }

  if (minStartFrame == std::numeric_limits<int>::max())
    return;

  const float leftX = mapper.timeToX(mapper.framesToSeconds(minStartFrame));
  const float rightX = mapper.timeToX(mapper.framesToSeconds(maxEndFrame));
  const float centerX = (leftX + rightX) * 0.5f;
  const float topY = mapper.midiToY(maxMidi);
  auto* targetNote = selectedNotes.front();
  // Use the actual rendered hover bounds when supplied by the piano roll.
  // Fall back to the note body plus its normal hover padding otherwise.
  constexpr float hoverPadding = 4.0f;
  const float fallbackWidth =
      std::max(rightX - leftX, 4.0f) + hoverPadding * 2.0f;
  const float layoutWidth = hoverBounds.isEmpty()
      ? std::max(fallbackWidth, buttonGroupWidth)
      : hoverBounds.getWidth();
  const float groupLeft = hoverBounds.isEmpty()
      ? centerX - layoutWidth * 0.5f
      : hoverBounds.getX();
  const float groupTop = hoverBounds.isEmpty()
      ? topY - hoverPadding - buttonHeight - 7.0f
      : hoverBounds.getY() - buttonHeight - 7.0f;
  const float slotWidth = (layoutWidth - buttonGap * 2.0f) / 3.0f;
  const float bottom = hoverBounds.isEmpty()
      ? topY + mapper.getPixelsPerSemitone() + hoverPadding
      : hoverBounds.getBottom();
  const float bottomRowY = bottom + 7.0f;
  const auto addButtonHandle = [this, targetNote, groupLeft, slotWidth](
      HandleType type, int column, float y)
  {
    const float x = groupLeft + (slotWidth + buttonGap) * column;
    addHandle(type, x + slotWidth * 0.5f, y + buttonHeight * 0.5f,
              targetNote);
    handles.back().bounds = {x, y, slotWidth, buttonHeight};
  };

  addButtonHandle(HandleType::TiltLeft, 0, groupTop);
  addButtonHandle(HandleType::Vibrato, 1, groupTop);
  addButtonHandle(HandleType::TiltRight, 2, groupTop);
  addButtonHandle(HandleType::PitchDrift, 0, bottomRowY);
  addButtonHandle(HandleType::Amplitude, 1, bottomRowY);
  addButtonHandle(HandleType::Formant, 2, bottomRowY);
}

void PitchToolHandles::draw(juce::Graphics& g) const {
  for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
    const auto& handle = handles[i];

    const bool hovered = i == hoveredHandleIndex;
    g.setColour(hovered ? juce::Colour(0xFF3B3B3Au)
                        : juce::Colour(0xFF2E2E2Du));

    const auto bounds = handle.bounds;
    g.fillRect(bounds);

    const auto& icon = getIconForType(handle.type);
    if (icon.isValid())
    {
      const float iconWidth = icon.getWidth() * 0.5f;
      const float iconHeight = icon.getHeight() * 0.5f;
      g.drawImage(icon, bounds.getCentreX() - iconWidth * 0.5f,
                  bounds.getCentreY() - iconHeight * 0.5f + 1.0f,
                  iconWidth, iconHeight, 0, 0, icon.getWidth(),
                  icon.getHeight());
    }
  }
}

const juce::Image&
PitchToolHandles::getIconForType(HandleType type) const {
  switch (type) {
    case HandleType::TiltLeft:
      return leftTiltIcon;
    case HandleType::PitchDrift:
      return driftIcon;
    case HandleType::Vibrato:
      return vibratoIcon;
    case HandleType::Formant:
      return formantIcon;
    case HandleType::Amplitude:
      return amplitudeIcon;
    case HandleType::TiltRight:
      return rightTiltIcon;
    default:
      return vibratoIcon;
  }
}

int PitchToolHandles::hitTest(float worldX, float worldY, float tolerance) const {
  for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
    if (handles[i].bounds.expanded(tolerance * 0.25f).contains(worldX, worldY)) {
      return i;
    }
  }
  return -1;
}

bool PitchToolHandles::containsLayoutPoint(float worldX, float worldY) const
{
  // Combine handles within each row, not across the note between rows.
  for (const auto& handle : handles)
  {
    auto rowBounds = handle.bounds;
    for (const auto& other : handles)
      if (other.bounds.getY() == handle.bounds.getY())
        rowBounds = rowBounds.getUnion(other.bounds);
    if (rowBounds.contains(worldX, worldY))
      return true;
  }
  return false;
}

juce::Rectangle<float> PitchToolHandles::getLayoutBounds() const
{
  if (handles.empty())
    return {};

  auto layoutBounds = handles.front().bounds;
  for (size_t i = 1; i < handles.size(); ++i)
    layoutBounds = layoutBounds.getUnion(handles[i].bounds);
  return layoutBounds;
}

void PitchToolHandles::addHandle(HandleType type, float worldX, float worldY, Note* note) {
  Handle h;
  h.type = type;
  h.note = note;
  h.color = getColorForType(type);
  
  h.bounds = juce::Rectangle<float>(worldX, worldY, 0.0f, 0.0f);
  
  handles.push_back(h);
}

juce::Colour PitchToolHandles::getColorForType(HandleType type) const {
  switch (type) {
    case HandleType::TiltLeft:
    case HandleType::TiltRight:
      return juce::Colours::orange;
      
    case HandleType::PitchDrift:
    case HandleType::Vibrato:
      return juce::Colours::mediumpurple;
      
    case HandleType::SmoothLeft:
    case HandleType::SmoothRight:
      return juce::Colours::cyan; // "Smooth" implies liquid/soft -> cyan/blue
      
    default:
      return juce::Colours::white;
  }
}
