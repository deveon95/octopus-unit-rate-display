# Octopus Unit Rate Display for ESP32

![photo of the hardware](images/finished-hardware.jpg)

This project gets JSON data from https://octopus.energy

The JSON data is parsed and used to get today's gas and electricity unit rates for the **Tracker** tariff and/or the current **Flexible** prices and/or the current **Agile** prices.

Prices are displayed on two or more 3-digit 7-segment displays to two decimal places if the unit rate is less than 10.00, or one decimal place if the unit rate is greater than or equal to 10.00. Values greater than or equal to 100.00 are displayed with no decimal places.

Negative prices are also supported. In such an event, the first digit will be used for the '-' character.

# Installation

You'll need to install esp-idf to build this project.

```
git clone https://github.com/deveon95/octopus-unit-rate-display
cd esp-idf-json/json-http-client2
chmod 777 getpem.sh
./getpem.sh
idf.py menuconfig
idf.py build flash monitor
```

The getpem.sh script gets the https certificate for octopus.energy. HTTPS is mandatory to use the Octopus API, and this step cannot be skipped! The script does work on Windows as well as Linux; you can run the script with Windows Subsystem for Linux, and you may already be able to run it without installing anything extra if you've installed Git for Windows.

# Configuration
The configuration is opened by running idf.py menuconfig.

You'll need to set the wifi SSID and password so that the ESP32 can connect to the internet, and the tariff codes for the most accurate prices. Note that the latest version of the code has a few extra options for tariff codes not shown in the old screenshots because it has support for more tariffs. The 'ENABLE' options that show up in the latest version of the code should be set to 1 for tariffs that are desired to be displayed or 0 for tariffs to exclude.

![config-top](images/Screenshot1.png)
![config-app](images/Screenshot2.png)

![finding the tariff on the ihd](images/ihd.jpg)

To see the console output, set the log verbosity to Info. To disable console output, set to None.

![config-log](images/Screenshot3.png)


# Console output   
When enabled in the configuration, the console output will show all the unit rates returned by the server, usually for every day of the current month.

# Hardware schematic
See the KiCad design. The board can be mostly assembled by JLCPCB with displays of your choosing added by hand later.

The board and firmware supports up to 8 3-digit 7-segment displays. The lower half of the board can be broken off to give a more compact 4 display version shown in the photo.

For further details of the project, please see [my blog](https://nick-elec.blogspot.com/2023/03/esp32-based-octopus-tracker-unit-rate.html)
