# User Stories
This doc explains how Smart Reel is going to be used. The goal is that this will help hone the direction of development.

## Background
The user uses Inventree to manage electronic components for PCB Assembly in a small to medium sized business or hobby environment. They use Inventree to streamline inventory management, and use plugins to automate monotonous tasks. For the sake of SmartReel, we're mostly concerned with reels of SMT components. Each reel in their library is individually labelled with a QR code that's associated with the Inventree StockItem.

## Background: Inventree inventory locations
Each SmartReel instance is configured as a structural inventory location in Inventree. Each slot is a sub location of the parent reel location, and when parts are stored, they are transferred into slot locations. This way, a user can lookup a part and see that it's in "SmartReel Rack A, Slot 32."

It's possible (though not recommended) that a part could be moved in Inventree without being removed from SmartReel first. If this happens, the SmartReel should alert the user so that the next time a user is present, the offending part(s) can be manually removed. This would work by lighting up the offending parts and instructing the user to remove them.

## Use case 1: storing parts
When the user receives new parts they've ordered, they inventory them into Inventree. This gets them into inventory, but they need to be properly stored. To do this, they walk up to the SmartReel HMI, then scan the reel's QR code, or clicks the load button on the screen and scans the QR code on the reel.

When this happens, the SmartReel instance uses its connection to Inventree via the SmartReel plugin to get basic info about the part, showing on the screen the name of the part and the current qty. It then follows the loading flow to direct the user to load it into a slot (see the loading flow section). Once placed, it moves that StockItem in Inventree to be housed in that slot location.

## Use case 2: browsing parts
The HMI has a view screen that lets you scroll through parts that are in the SmartReel unit. Each entry has a find button that when pressed lights up the LEDs for that slot for a period of a few seconds. There is also a pick button that triggers a single pick action.

## User case 3: single part picking
When a user wants to retrieve a single reel from SmartReel, they go to the view screen, find the part in the list, then click pick. The system then lights up the LED corresponding to that part's location. At any time prior to the part being removed the user can cancel the action by clicking a cancel button on the HMI. Once the user picks up the reel, the button is no longer pressed, and the system registers the inventory removal. This causes the reel StockItem to be transferred to a predetermined inventory location, i.e. "Staging" in Inventree. This location is configured in the Inventree plugin. Once complete, it turns off the LED and the slot is now logically empty.

## User case 4: job picking
When the user has a build order for parts in Inventree, they can open the SmartReel plugin and send a pick job. This basically provides the smart reel unit a "job" which is a series of parts that need to be picked. The user can then go to the jobs screen in the HMI, select the job, and the unit will light up all the parts that need to be picked. Once all the parts are picked, the job is complete and all the parts are transferred (to the staging location or another specificed location).

If the user only picks some of the parts, or none of the parts, and presses the cancel button, then only the parts (if any) that were removed are moved in inventory. The user can come back to finish the pick job later (it should show partially picked).

## SmartReel hardware
SmartReel is composed of reel modules. These have a button for each slot that is pressed whenever a reel is present in the slot. Additionally there are dividers that can be removed to make slots wider, to accomodate larger reels. Each divider has a button that is pressed when the divider is present. This allows SmartReel to automatically know the width of slots (single width, double, multi, etc), and the presence of parts. Each slot also has an addressable RGB led. This is used to indicate slot status during various actions. When a composite slot is formed, all of the LEDs for the individual slots in that group all follow the same state together. Like if a slot needs to light up red and there's three slots in the composite slot, all three LEDs should be red.

## SmartReel Loading
This details exactly how loading works from a user interaction perspective. Once the HMI is ready to load, it lights up the LEDs for any slots that are not currently occupied. These are potential locations for the user to place the part. The user can cancel the loading operation at any time prior to inserting the parts by clicking cancel on the HMI. Once they place the reel of parts into a slot, the HMI detects the button being pressed and registers that location as the location of that part. The part is now "placed" and the loading action is complete, and inventory records can be updated.


## Internet connectivity
The HMI needs internet connectivity to connect to Inventree and the SmartReel plugin. If internet is not present, the user should be warned, and we should try to re-establish a connection. For now, we won't support picking if access to Inventree isn't available, to prevent the Smartreel from getting out of sync with Inventree.

## UI Updates
A last few notes on improving the UI.
The current screen has very small font. It's only a 4.3" screen, so we should prioritize less, larger font size text. The current reel occupancy grid that takes up the top 1/3 of the home screen should move to its own screen. We should try to keep all font sizes at least as large as what we have for the main card titles on the home screen. The current small font size is too small.

