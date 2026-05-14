#pragma once

#include "../JuceHeader.h"

/**
 * Centralized command IDs for PitchNet.
 * 
 * IDs start at 0x2000 to avoid conflicts with JUCE's StandardApplicationCommandIDs.
 * JUCE reserves 0x0001-0x0FFF for internal use.
 */
namespace CommandIDs
{
    enum
    {
        // File Menu Commands (0x2000-0x200F)
        openFile            = 0x2001,
        saveProject         = 0x2002,
        exportAudio         = 0x2003,
        exportMidi          = 0x2004,
        quit                = 0x2005,
        
        // Edit Menu Commands (0x2010-0x201F)
        undo                = 0x2010,
        redo                = 0x2011,
        selectAll           = 0x2012,
        
        // View Menu Commands (0x2020-0x202F)
        showDeltaPitch      = 0x2020,
        showBasePitch       = 0x2021,
        showSettings        = 0x2022,
        
        // Transport Commands (0x2030-0x203F)
        playPause           = 0x2030,
        stop                = 0x2031,
        goToStart           = 0x2032,
        goToEnd             = 0x2033,
        
        // Edit Mode Commands (0x2040-0x204F)
        toggleDrawMode      = 0x2040,
        exitDrawMode        = 0x2041,
        
        // Pitch Tool Commands (0x2050-0x205F)
        fourierFilter       = 0x2050
    };
}
