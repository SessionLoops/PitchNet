#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Note.h"
#include <vector>

/**
 * Exports Note data to standard MIDI files.
 *
 * Design principles:
 * - Pure export logic, no UI dependencies
 * - Uses adjusted pitch data (respects user edits)
 * - Stateless utility class
 */
struct MidiExportOptions {
  int ticksPerQuarterNote = 480; // MIDI resolution (PPQ)
  float tempo = 120.0f;          // BPM
  int channel = 0;               // MIDI channel (0-15)
  int velocity = 100;            // Default velocity (0-127)
  bool includeTempoTrack = true; // Add tempo meta event
  bool quantizePitch = true;     // Round pitch to nearest semitone
};

class MidiExporter {
public:
  using ExportOptions = MidiExportOptions;

  static bool hasExportableNotes(const std::vector<Note> &notes);

  /**
   * Export notes to a MIDI file.
   *
   * @param notes     The notes to export (uses adjusted pitch)
   * @param file      Output file path
   * @param options   Export settings
   * @return true if export succeeded
   */
  static bool exportToFile(const std::vector<Note> &notes,
                           const juce::File &file,
                           const ExportOptions &options = ExportOptions());

  /**
   * Create a MidiFile object from notes (for further manipulation).
   *
   * @param notes     The notes to convert
   * @param options   Export settings
   * @return MidiFile object ready for writing
   */
  static juce::MidiFile
  createMidiFile(const std::vector<Note> &notes,
                 const ExportOptions &options = ExportOptions());

private:
  // Convert frame index to MIDI ticks
  static int frameToTicks(int frame, float tempo, int ppq);

  // Convert seconds to MIDI ticks
  static int secondsToTicks(double seconds, float tempo, int ppq);

  // Clamp MIDI note to valid range (0-127)
  static int clampMidiNote(float midiNote);

  MidiExporter() = delete; // Static-only class
};
