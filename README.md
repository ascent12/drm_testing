This is just some testing code for using Linux's DRM API with EGL and GBM.
Don't expect it to be high quality.
It might be useful for someone who just wants to see a working example.
Currently, it will display some colours to each of your monitors.

What it currently **DOESN'T** handle:
- Display hotplugging
- Virtual terminal switching
- Session management
- Custom modesetting

# Usage
Switch to a new virtual terminal (e.g. Ctrl+Alt+F2)
```
make
./test
```
Press enter to close it.
