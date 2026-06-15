View and Rack don't make intuitive sense as to the difference between them. Can we make Rack a sub page of view?
When a reel is removed illegally, light up the slot it was removed from. Maybe flash the LED red.
Reel was unloaded in inventory when it was illegally moved and replaced and I clicked resolve. You shouldn't be able to inventory unload it illegally, there should be another layer of explicit confirmation, like "are you sure you want to unload this part from inventory?"
Show confirmation on load that it was loaded correctly, maybe show the LED for that slot for 2 secs too (green?).
Add beeper hardware (I'll do this, then we'll wire confirmation and error actions to beep tones).
Doesn't appear to detect when a part was moved in Inventree out of the slot to staging and reconciliation is needed. Needs to detect this within 10 secs.
Add a confirmation on the completion of a manual pick.
Refresh button maybe didn't work on pick screen. Add confirmation when refresh happens or completes.

Build order plugin screen should better handle multiple smartreels. Maybe let me select which stockitems I want to pull (if there's multiple for a given part), then send it to all smartreels that have parts in the list.
What happens if BO line text is too long?
We should order pick job line items sequentially by their slot numbers.
The pick job screen picked button is confusing, get rid of it. there should be a status that goes green when done for each line.
Qty for line items on the pick job screen doesn't matter, get rid of it.
Once fully picked, remove the ability to cancel the job. What does cancel job do?
When you click pick on a line from the view page, a pop up should appear telling you to remove the reel, then once removed, it confirms and closes.
Rename "scan part barcode" to "scan reel barcode" on load screen.
Reconcile occupancy with physical buttons. If the system boots and there's a slot occupied in inventree inventory, but none of the buttons for it are pressed, this needs to be handled and resolved. I've had issues with phantom parts remaining (like an occupied slot, but no parts are physically present).
On load screen, make "place reel in any lit spot" be the top line and much much bigger font size.
Show scan error on load screen for longer, maybe 2 sec delay, or a text view with the most recent errors?
Will it accept a reel currently loaded in a different smart reel instance?