Android Serial GPS Driver

How to use:

To set serial port, add a property "ro.kernel.android.gps" and set it equal to your GPS device file.
ie. ro.kernel.android.gps=ttyO1

Default baud rate is 9600, to adjust add a property "ro.kernel.android.gpsttybaud" and set it equal to the needed rate. (4800-115200)
ie. ro.kernel.android.gpsttybaud=9600

Default GPS fix rate (interval between position updates) depends on the used device but it is often 1 second.  This rate is possible
to modify by property "ro.kernel.android.gps.max_rate" if your GPS device is able to process UBX protocol. Acceptable property value
must be from one of two ranges - from 1 to 65 seconds or from 250 to 65000 milliseconds.

Serial GPS driver is able to synchronize system time by the similar way as Internet connection. Property
"ro.kernel.android.gps.time_sync" which is greater than zero is taken as threshold in seconds. If difference between system time
and GPS time will grow over given threshold then system time will be set to GPS time with the respect to the current time zone.
Threshold should be set to some value from 20 to 60. If property is missing or its value is zero then time synchronization
is turned off.

Notes:
* If using a USB device make sure you have the necessary kernel modules loaded or built in to the kernel.
* Make sure the permissions on your device file are correct

Donate:
If you find any of this useful and want to show appreciation see below:

PayPal: keith.conger@gmail.com
Bitcoin: 1Pg54vVnaLxNsziA6cy9CTefoEG5iAm9Uh
