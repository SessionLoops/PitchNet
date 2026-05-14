#include "PitchToolHandles.h"

PitchToolHandles::PitchToolHandles() {
  // Initialize (currently empty, but reserve space)
  handles.reserve(20);  // Typical max handles for multi-note selection
}

void PitchToolHandles::updateHandles(const std::vector<Note*>& selectedNotes,
                                     const CoordinateMapper& mapper) {
  juce::ignoreUnused(selectedNotes, mapper);
  handles.clear();
}

void PitchToolHandles::draw(juce::Graphics& g) const {
  for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
    const auto& handle = handles[i];

    g.setColour(handle.color);
    
    auto drawBounds = handle.bounds;
    
    // Draw filled circle
    g.fillEllipse(drawBounds);
    
    // Draw outline for better visibility against note backgrounds
    g.setColour(handle.color.darker(0.3f));
    g.drawEllipse(drawBounds, 1.0f);
  }
}

int PitchToolHandles::hitTest(float worldX, float worldY, float tolerance) const {
  for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
    auto center = handles[i].bounds.getCentre();
    float distance = center.getDistanceFrom(juce::Point<float>(worldX, worldY));
    
    if (distance <= tolerance) {
      return i;
    }
  }
  return -1;
}

void PitchToolHandles::addHandle(HandleType type, float worldX, float worldY, Note* note) {
  Handle h;
  h.type = type;
  h.note = note;
  h.color = getColorForType(type);
  
  // Center the handle bounds on the coordinate (now in world space)
  float halfSize = HANDLE_SIZE * 0.5f;
  h.bounds = juce::Rectangle<float>(worldX - halfSize, worldY - halfSize, 
                                   HANDLE_SIZE, HANDLE_SIZE);
  
  handles.push_back(h);
}

juce::Colour PitchToolHandles::getColorForType(HandleType type) const {
  switch (type) {
    case HandleType::TiltLeft:
    case HandleType::TiltRight:
      return juce::Colours::orange;
      
    case HandleType::ReduceVariance:
      return juce::Colours::mediumpurple; // "Reduce" implies constraint -> purple/magenta
      
    case HandleType::SmoothLeft:
    case HandleType::SmoothRight:
      return juce::Colours::cyan; // "Smooth" implies liquid/soft -> cyan/blue
      
    default:
      return juce::Colours::white;
  }
}
