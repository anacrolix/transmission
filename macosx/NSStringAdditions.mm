/******************************************************************************
 * Copyright (c) 2005-2019 Transmission authors and contributors
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

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h>

#import "NSApplicationAdditions.h"
#import "NSStringAdditions.h"

@interface NSString (Private)

+ (NSString*)stringForFileSizeLion:(uint64_t)size showUnitUnless:(NSString*)notAllowedUnit unitsUsed:(NSString**)unitUsed;

+ (NSString*)stringForSpeed:(CGFloat)speed kb:(NSString*)kb mb:(NSString*)mb gb:(NSString*)gb;

@end

@implementation NSString (NSStringAdditions)

+ (NSString*)ellipsis
{
    return @"\xE2\x80\xA6";
}

- (NSString*)stringByAppendingEllipsis
{
    return [self stringByAppendingString:NSString.ellipsis];
}

#warning use localizedStringWithFormat: directly when 10.9-only and stringsdict translations are in place
+ (NSString*)formattedUInteger:(NSUInteger)value
{
    return [NSString localizedStringWithFormat:@"%lu", value];
}

#warning should we take long long instead?
+ (NSString*)stringForFileSize:(uint64_t)size
{
    return [NSByteCountFormatter stringFromByteCount:size countStyle:NSByteCountFormatterCountStyleFile];
}

#warning should we take long long instead?
+ (NSString*)stringForFilePartialSize:(uint64_t)partialSize fullSize:(uint64_t)fullSize
{
    NSByteCountFormatter* fileSizeFormatter = [[NSByteCountFormatter alloc] init];

    NSString* fullString = [fileSizeFormatter stringFromByteCount:fullSize];

    //figure out the magniture of the two, since we can't rely on comparing the units because of localization and pluralization issues (for example, "1 byte of 2 bytes")
    BOOL partialUnitsSame;
    if (partialSize == 0)
    {
        partialUnitsSame = YES; //we want to just show "0" when we have no partial data, so always set to the same units
    }
    else
    {
        unsigned int const magnitudePartial = log(partialSize) / log(1000);
        // we have to catch 0 with a special case, so might as well avoid the math for all of magnitude 0
        unsigned int const magnitudeFull = fullSize < 1000 ? 0 : log(fullSize) / log(1000);
        partialUnitsSame = magnitudePartial == magnitudeFull;
    }

    fileSizeFormatter.includesUnit = !partialUnitsSame;
    NSString* partialString = [fileSizeFormatter stringFromByteCount:partialSize];

    return [NSString stringWithFormat:NSLocalizedString(@"%@ of %@", "file size string"), partialString, fullString];
}

+ (NSString*)stringForSpeed:(CGFloat)speed
{
    return [self stringForSpeed:speed kb:NSLocalizedString(@"KB/s", "Transfer speed (kilobytes per second)")
                             mb:NSLocalizedString(@"MB/s", "Transfer speed (megabytes per second)")
                             gb:NSLocalizedString(@"GB/s", "Transfer speed (gigabytes per second)")];
}

+ (NSString*)stringForSpeedAbbrev:(CGFloat)speed
{
    return [self stringForSpeed:speed kb:@"K" mb:@"M" gb:@"G"];
}

+ (NSString*)stringForRatio:(CGFloat)ratio
{
    //N/A is different than libtransmission's
    if ((int)ratio == TR_RATIO_NA)
    {
        return NSLocalizedString(@"N/A", "No Ratio");
    }
    else if ((int)ratio == TR_RATIO_INF)
    {
        return @"\xE2\x88\x9E";
    }
    else
    {
        if (ratio < 10.0)
        {
            return [NSString localizedStringWithFormat:@"%.2f", tr_truncd(ratio, 2)];
        }
        else if (ratio < 100.0)
        {
            return [NSString localizedStringWithFormat:@"%.1f", tr_truncd(ratio, 1)];
        }
        else
        {
            return [NSString localizedStringWithFormat:@"%.0f", tr_truncd(ratio, 0)];
        }
    }
}

+ (NSString*)percentString:(CGFloat)progress longDecimals:(BOOL)longDecimals
{
    if (progress >= 1.0)
    {
        return [NSString localizedStringWithFormat:@"%d%%", 100];
    }
    else if (longDecimals)
    {
        return [NSString localizedStringWithFormat:@"%.2f%%", tr_truncd(progress * 100.0, 2)];
    }
    else
    {
        return [NSString localizedStringWithFormat:@"%.1f%%", tr_truncd(progress * 100.0, 1)];
    }
}

- (NSComparisonResult)compareNumeric:(NSString*)string
{
    NSStringCompareOptions const comparisonOptions = NSNumericSearch | NSForcedOrderingSearch;
    return [self compare:string options:comparisonOptions range:NSMakeRange(0, self.length) locale:NSLocale.currentLocale];
}

- (NSArray*)betterComponentsSeparatedByCharactersInSet:(NSCharacterSet*)separators
{
    NSMutableArray* components = [NSMutableArray array];

    NSCharacterSet* includededCharSet = separators.invertedSet;
    NSUInteger index = 0;
    NSUInteger const fullLength = self.length;
    do
    {
        NSUInteger const start = [self rangeOfCharacterFromSet:includededCharSet options:0
                                                         range:NSMakeRange(index, fullLength - index)]
                                     .location;
        if (start == NSNotFound)
        {
            break;
        }

        NSRange const endRange = [self rangeOfCharacterFromSet:separators options:0 range:NSMakeRange(start, fullLength - start)];
        if (endRange.location == NSNotFound)
        {
            [components addObject:[self substringFromIndex:start]];
            break;
        }

        [components addObject:[self substringWithRange:NSMakeRange(start, endRange.location - start)]];

        index = NSMaxRange(endRange);
    } while (YES);

    return components;
}

@end

@implementation NSString (Private)

+ (NSString*)stringForFileSizeLion:(uint64_t)size showUnitUnless:(NSString*)notAllowedUnit unitsUsed:(NSString**)unitUsed
{
    double convertedSize;
    NSString* unit;
    NSUInteger decimals;
    if (size < pow(1000, 2))
    {
        convertedSize = size / 1000.0;
        unit = NSLocalizedString(@"KB", "File size - kilobytes");
        decimals = convertedSize >= 10.0 ? 0 : 1;
    }
    else if (size < pow(1000, 3))
    {
        convertedSize = size / powf(1000.0, 2);
        unit = NSLocalizedString(@"MB", "File size - megabytes");
        decimals = 1;
    }
    else if (size < pow(1000, 4))
    {
        convertedSize = size / powf(1000.0, 3);
        unit = NSLocalizedString(@"GB", "File size - gigabytes");
        decimals = 2;
    }
    else
    {
        convertedSize = size / powf(1000.0, 4);
        unit = NSLocalizedString(@"TB", "File size - terabytes");
        decimals = 2;
    }

    //match Finder's behavior
    NSNumberFormatter* numberFormatter = [[NSNumberFormatter alloc] init];
    numberFormatter.numberStyle = NSNumberFormatterDecimalStyle;
    numberFormatter.minimumFractionDigits = 0;
    numberFormatter.maximumFractionDigits = decimals;

    NSString* fileSizeString = [numberFormatter stringFromNumber:@(convertedSize)];

    if (!notAllowedUnit || ![unit isEqualToString:notAllowedUnit])
    {
        fileSizeString = [fileSizeString stringByAppendingFormat:@" %@", unit];
    }

    if (unitUsed)
    {
        *unitUsed = unit;
    }

    return fileSizeString;
}

+ (NSString*)stringForSpeed:(CGFloat)speed kb:(NSString*)kb mb:(NSString*)mb gb:(NSString*)gb
{
    if (speed <= 999.95) //0.0 KB/s to 999.9 KB/s
    {
        return [NSString localizedStringWithFormat:@"%.1f %@", speed, kb];
    }

    speed /= 1000.0;

    if (speed <= 99.995) //1.00 MB/s to 99.99 MB/s
    {
        return [NSString localizedStringWithFormat:@"%.2f %@", speed, mb];
    }
    else if (speed <= 999.95) //100.0 MB/s to 999.9 MB/s
    {
        return [NSString localizedStringWithFormat:@"%.1f %@", speed, mb];
    }
    else //insane speeds
    {
        return [NSString localizedStringWithFormat:@"%.2f %@", (speed / 1000.0), gb];
    }
}

@end
