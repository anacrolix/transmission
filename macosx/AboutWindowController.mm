/******************************************************************************
 * Copyright (c) 2007-2012 Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include <libtransmission/version.h>

#import "AboutWindowController.h"

@implementation AboutWindowController

AboutWindowController* fAboutBoxInstance = nil;

+ (AboutWindowController*)aboutController
{
    if (!fAboutBoxInstance)
    {
        fAboutBoxInstance = [[self alloc] initWithWindowNibName:@"AboutWindow"];
    }
    return fAboutBoxInstance;
}

- (void)awakeFromNib
{
    fVersionField.stringValue = @(LONG_VERSION_STRING);

    fCopyrightField.stringValue = [NSBundle.mainBundle localizedStringForKey:@"NSHumanReadableCopyright" value:nil
                                                                       table:@"InfoPlist"];

    [fTextView.textStorage setAttributedString:[[NSAttributedString alloc] initWithPath:[NSBundle.mainBundle pathForResource:@"Credits"
                                                                                                                      ofType:@"rtf"]
                                                                     documentAttributes:nil]];

    //size license button
    CGFloat const oldButtonWidth = NSWidth(fLicenseButton.frame);

    fLicenseButton.title = NSLocalizedString(@"License", "About window -> license button");
    [fLicenseButton sizeToFit];

    NSRect buttonFrame = fLicenseButton.frame;
    buttonFrame.size.width += 10.0;
    buttonFrame.origin.x -= NSWidth(buttonFrame) - oldButtonWidth;
    fLicenseButton.frame = buttonFrame;
}

- (void)windowDidLoad
{
    [self.window center];
}

- (void)windowWillClose:(id)sender
{
    fAboutBoxInstance = nil;
}

- (IBAction)showLicense:(id)sender
{
    NSString* licenseText = [NSString stringWithContentsOfFile:[NSBundle.mainBundle pathForResource:@"COPYING" ofType:nil]
                                                  usedEncoding:nil
                                                         error:NULL];
    fLicenseView.string = licenseText;
    fLicenseCloseButton.title = NSLocalizedString(@"OK", "About window -> license close button");

    [self.window beginSheet:fLicenseSheet completionHandler:nil];
}

- (IBAction)hideLicense:(id)sender
{
    [fLicenseSheet orderOut:nil];
    [NSApp endSheet:fLicenseSheet];
}

@end
