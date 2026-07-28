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

const juce::Image& iconFor(PitchToolHandles::HandleType type)
{
  static const auto leftTilt = loadIcon("ltilt_png");
  static const auto vibrato = loadIcon("vibrato_png");
  static const auto rightTilt = loadIcon("rtilt_png");
  switch (type)
  {
    case PitchToolHandles::HandleType::TiltLeft: return leftTilt;
    case PitchToolHandles::HandleType::Vibrato: return vibrato;
    case PitchToolHandles::HandleType::TiltRight: return rightTilt;
    default: return vibrato;
  }
}

} // namespace

PitchToolHandles::PitchToolHandles() {
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
      ? topY - buttonHeight - 7.0f
      : hoverBounds.getY() - buttonHeight - 7.0f;
  const float slotWidth = (layoutWidth - buttonGap * 2.0f) / 3.0f;
  const auto addButtonHandle = [this, targetNote, groupTop](HandleType type,
                                                             float x, float width)
  {
    addHandle(type, x + width * 0.5f, groupTop + buttonHeight * 0.5f,
              targetNote);
    handles.back().bounds = {x, groupTop, width, buttonHeight};
  };

  addButtonHandle(HandleType::TiltLeft, groupLeft, slotWidth);
  addButtonHandle(HandleType::Vibrato, groupLeft + slotWidth + buttonGap,
                  slotWidth);
  addButtonHandle(HandleType::TiltRight,
                  groupLeft + (slotWidth + buttonGap) * 2.0f, slotWidth);
}

void PitchToolHandles::draw(juce::Graphics& g) const {
  for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
    const auto& handle = handles[i];

    const bool hovered = i == hoveredHandleIndex;
    g.setColour(hovered ? juce::Colour(0xFF3B3B3Au)
                        : juce::Colour(0xFF2E2E2Du));

    const auto bounds = handle.bounds;
    if (handle.type == HandleType::Vibrato)
    {
      g.fillRect(bounds);
    }
    else
    {
      constexpr float slant = 7.0f;
      constexpr float radius = 4.0f;
      juce::Path path;
      if (handle.type == HandleType::TiltLeft)
      {
        path.startNewSubPath(bounds.getX() + slant, bounds.getY());
        path.lineTo(bounds.getRight() - radius, bounds.getY());
        path.cubicTo(bounds.getRight() - radius * 0.45f, bounds.getY(),
                     bounds.getRight(), bounds.getY() + radius * 0.45f,
                     bounds.getRight(), bounds.getY() + radius);
        path.lineTo(bounds.getRight(), bounds.getBottom());
        path.lineTo(bounds.getX(), bounds.getBottom());
        path.lineTo(bounds.getX(), bounds.getY() + radius);
        path.cubicTo(bounds.getX(), bounds.getY() + radius * 0.55f,
                     bounds.getX() + slant * 0.35f, bounds.getY(),
                     bounds.getX() + slant, bounds.getY());
      }
      else
      {
        path.startNewSubPath(bounds.getX() + radius, bounds.getY());
        path.lineTo(bounds.getRight() - slant, bounds.getY());
        path.cubicTo(bounds.getRight() - slant * 0.35f, bounds.getY(),
                     bounds.getRight(), bounds.getY() + radius * 0.55f,
                     bounds.getRight(), bounds.getY() + radius);
        path.lineTo(bounds.getRight(), bounds.getBottom());
        path.lineTo(bounds.getX(), bounds.getBottom());
        path.lineTo(bounds.getX(), bounds.getY() + radius);
        path.cubicTo(bounds.getX(), bounds.getY() + radius * 0.45f,
                     bounds.getX() + radius * 0.45f, bounds.getY(),
                     bounds.getX() + radius, bounds.getY());
      }
      path.closeSubPath();
      g.fillPath(path);
    }

    const auto& icon = iconFor(handle.type);
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
  return getLayoutBounds().contains(worldX, worldY);
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
      
    case HandleType::Vibrato:
      return juce::Colours::mediumpurple;
      
    case HandleType::SmoothLeft:
    case HandleType::SmoothRight:
      return juce::Colours::cyan; // "Smooth" implies liquid/soft -> cyan/blue
      
    default:
      return juce::Colours::white;
  }
}
