/* ui_icons.h — the icons this window uses, by their Phosphor names.
 *
 * Phosphor ships as a font, which suits a program that already has a
 * rasterizer and a glyph atlas: an icon is a codepoint in a private-use
 * block, drawn through the same path as a letter. No SVG parser, no
 * second kind of image, nothing to pack at build time.
 *
 * The codepoints come from the family's own stylesheet (the `content`
 * of each .ph-<name>:before rule). They are listed here with the names
 * they go by on phosphoricons.com, so swapping one is a matter of
 * looking it up there and changing the number.
 *
 * vendor/Phosphor.ttf, v2.1.1, MIT — see vendor/Phosphor-LICENSE.txt.
 */

#ifndef UI_ICONS_H
#define UI_ICONS_H

#define UI_ICON_ARROW_RIGHT 0xE06C   /* ph-arrow-right:    send           */
#define UI_ICON_UPLOAD 0xE4C0        /* ph-upload-simple:  import a file  */
#define UI_ICON_IMAGE 0xE2CA         /* ph-image:          a photo        */
#define UI_ICON_EAR 0xE70C           /* ph-ear:            overheard      */
#define UI_ICON_CHAT 0xE16C          /* ph-chat-circle-dots: no friend    */
#define UI_ICON_GEAR 0xE270          /* ph-gear:           settings       */
#define UI_ICON_ARCHIVE 0xE00C       /* ph-archive:        out of the list */
#define UI_ICON_DOWNLOAD 0xE20C      /* ph-download-simple: export a friend */
#define UI_ICON_PLUS 0xE3D4          /* ph-plus:           a new friend,
                                      *                    a photograph  */
#define UI_ICON_X 0xE4F6             /* ph-x:              dismiss       */
#define UI_ICON_MIC 0xE326           /* ph-microphone:     talk to it    */
#define UI_ICON_MIC_OFF 0xE328       /* ph-microphone-slash: stop        */

#endif /* UI_ICONS_H */
