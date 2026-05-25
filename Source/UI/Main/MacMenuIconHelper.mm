#include "MacMenuIconHelper.h"

#if JUCE_MAC
#import <AppKit/AppKit.h>

namespace MacMenuIconHelper {

void applySettingsMenuIcon(const juce::String& settingsTitle) {
  NSMenu* mainMenu = [NSApp mainMenu];
  if (mainMenu == nil || [mainMenu numberOfItems] == 0)
    return;

  NSMenu* appMenu = [[mainMenu itemAtIndex:0] submenu];
  if (appMenu == nil)
    return;

  NSString* title = [NSString stringWithUTF8String:settingsTitle.toRawUTF8()];
  NSImage* icon = nil;

  if (@available(macOS 11.0, *))
    icon = [NSImage imageWithSystemSymbolName:@"gearshape" accessibilityDescription:title];
  else
    icon = [NSImage imageNamed:NSImageNameActionTemplate];

  if (icon == nil)
    return;

  for (NSMenuItem* item in [appMenu itemArray]) {
    if ([[item title] isEqualToString:title]) {
      [item setImage:icon];
      return;
    }
  }
}

} // namespace MacMenuIconHelper
#endif
