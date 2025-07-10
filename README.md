<img width="1262" height="1690" alt="traffic light" src="https://github.com/user-attachments/assets/a769b357-240f-4adc-b90d-a017af062670" />

# Setup

<details>

  <summary><h2> ESP32-CAM <h2/></summary>

  ### Add esp32 boards
  Add https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json to:
  <br>
  menu bar > File > preferences <br>
  <img width="799" height="529" alt="image" src="https://github.com/user-attachments/assets/12f1371d-5a36-4ec8-ad13-3db73c7f2a1f" /> <br>
  now go to the board manager tab and install esp32 by espressif systems<br>
  <img width="267" height="466" alt="image" src="https://github.com/user-attachments/assets/e1663e57-c35e-42fb-94ac-4cb841237a83" /> <br> 
  ### Install esp32-cam library
  download https://github.com/yoursunny/esp32cam as zip and in arduino IDE <br>
  menu bar > sketch > Include library > Add .zip library > select downloaded zip <br>
  <img width="603" height="339" alt="image" src="https://github.com/user-attachments/assets/fa7fe9dc-61f0-4fbb-ae25-d6c71e1b5beb" />
  <hr/>
  after all this, select board `AI Thinker ESP32-CAM` and upload `arduino IDE\esp32cam\esp32cam.ino` to the esp32cam<br>
  possible resoltions are 160x120, 176x144, 240x176, 320x240, 400x296, 640x480, 800x600, 1024x768, 1280x1024, 1600x1200, change according to preference in the code
</details>

<hr/>
<details>
<summary><h2>Arduino</h1></summary>
Install `AccelStepper` library and change stepper and traffic light pins according to your own wiring<br>
Upload code
</details>

<hr/>
<details>
<summary><h2>Computer</h1></summary>
Install python from https://www.python.org/downloads/ and make sure to check `Add to PATH` during installation<br>
Go to the upper most directory of this project and where all folders can be seen and run<br>
`pip install -r requirements.txt` to install all the required python libraries
</details>

# Running
  ## ESP32  
  open serial monitor at 115200, it will ask for wifi name and password, after connecting it will print the page link where it updates the camera stream
  ## Arduino
  open serial monitor at 9600, try different command likes T, R, C1, etc to check working of all components
  close serial monitor
  ### computer
  change image url at line 18 in traffic_control.py according to the one given by the esp32
  go to the Traffic Monitor folder and open cmd, run `py traffic_control.py` 

  
