This project was created to monitor the real-time operating status of vibration sensors. Rather than being a fully completed product, it is more accurately described as a **development framework** designed to support one or multiple sensors using the **CAN or Modbus communication protocol**.

The framework supports real-time display of vibration values and waveforms on a screen. Users can set vibration thresholds using physical buttons, and the system provides **LED and buzzer alarm mechanisms** when abnormal conditions are detected. The current operating status can also be displayed in real time, indicating whether the system is operating normally. In addition, the system supports **firmware/software upgrade functionality**.

As a development framework, the project adopts a **layered architecture**, with different functions separated into independent modules. This structure makes the system easier to extend, upgrade, debug, and maintain over the long term.

To support different hardware configurations, the project uses a **Hardware Configuration Table** to separate hardware-specific settings from the application logic. The current implementation uses:

* One ST7565R LCD display
* Six physical buttons
* Four LED indicators, including one RGB LED and three single-color LEDs
* One buzzer

The framework is designed to be highly adaptable to different hardware configurations. When developing a similar project based on this framework:

* The display can be replaced with a different type of display.
* The number of buttons can be increased or reduced without imposing a fixed limitation.
* Buttons, LEDs, and buzzers can be configured as either **active-high or active-low** devices.
* The type and number of LEDs and buzzers can be changed as required.
* An RGB LED can be treated as three independent single-color LEDs.
* LEDs and buzzers are abstracted as the same type of controllable output device.

When different hardware is used, only the corresponding hardware-specific modules need to be rewritten. The rest of the project does not need to be modified, which significantly improves the **portability, scalability, and maintainability** of the framework.


这是为了检测(震动)传感器实时运行状态而创建的项目，比起说这是一个完成的项目，不如说这是一套框架，适用于同时连接一个(或多个)采用CAN(或Modbus)协议的传感器。
该项目支持屏幕实时显示震动数值与图像，并且可使用按钮设置震动阈值，拥有LED与蜂鸣器报警机制，并可实时显示运行状态是否正常，可通过软件实现升级功能。
该项目作为一套开发框架，采用了分层结构，各个功能各司其职，便于后期升级与长期维护。
为了适配不同的硬件，该项目使用“硬件配置表”实现了软硬件分离。该项目目前使用了一个st7565r LCD显示屏，六个按钮，四个LED指示灯(1个三色灯，三个单色灯)以及一个蜂鸣器。
使用本项目进行开发类似项目，可以自行更换显示屏，可以增加或减少按钮数量，不限制按钮，LED与蜂鸣器的触发方式(高电平触发与低电平触发)，LED与蜂鸣器可更换种类与数量(三色灯可看作三个单色灯),蜂鸣器和LED看作同种设备。
更换不同硬件，只需重新编写对应的模块即可，无需修改整个项目。
