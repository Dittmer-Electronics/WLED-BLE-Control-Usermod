I'm creating a WLED usermod to allow the user to change presets and toggle power from a WearOS device.

There are two parts to this project: the WLED Usermod (in it's own repo) and the WearOS application (in a seperate repo to the usermod).

The features of the project are as follows:
- User will launch the app and search for available devices. Ideally there should be some sort of security/verification to stop hijacking of other devices.
- Connected devices are stored for fast access next time.
- On connect, the user will be shown the name of the device and the controls:
  - A power toggle that updates when the WLED power is toggled.
  - The list of available presets by name, of course making it clear which is currently selected/running.
  - A button to disconnect.
- The WearOS app also provides relevant tiles and complications. Any complications or tile buttons that have actions should attempt to connect to the last connected controller and make the action, of course indicating appropriately if that was not possible. Likewise with reading complications and tile elements they should update on property change while connected. Here are examples of complications and tile elements that could be useful, add more if you think they would be beneficial:
  - Fast toggle complication.
  - Display current preset.
  - Display connected status (with device name).
  - One click turn off and disconnect.

Consider this the prompt for the whole project that resides in the usermod's repo. I intend to make the WearOS app in a seperate repo, so once the decisions have been made regarding protocols, generate a prompt for another thread (or ask me for the path to the other repo if you can do it all in one thread).
I intend to upload the app to the Google Play store and make the WLED usermod available publicly.
Please ask as many follow up questions as required.
Update readme files as appropriate, and update the .gitignore as appropriate too. Also add a licence to each repo - due to it's simplicity, I'm happy for the code to be used publicly for private and commercial use provided any modifications are public and attributed to the original (same sort of philosophy as the main WLED project).