# watson4x4_mqtt

Watson box project

This project name derives from the story of Alexander Graham Bell's first words over his new telepone (March 10, 1876) which were "Mr. Watson, come here -- I want to see you"


This project was realized, during the early COVID shutdown,  as a pair of boxes each with 4 LEDs and 4 buttons.  Pushing a button toggles the state of the corresponding LED on both boxes.  The purpose is to signal to the person with the other box that attention is needed (in case they are out of hearing range or have their headphones on).

---

### state machine
| init  |       | action   |       | yields      | mqtt state |
| ----- | :---: | -------- | :---: | ----------- | ---------- |
| off   | -->   | push     | -->   | on (steady) | '1' |
| off   | -->   | double   | -->   | slow blink  | 'S' |
| off   | -->   | triple   | -->   | fast blink  | 'F' |
| off   | -->   | long     | -->   | breathe     | 'B' |
| ! off | -->   | any push | -->   | off         | '0' |
(other possible state 'X' super fast blink, but not settable by button)
 
State is kept via MQTT communication and retained messages
| topic                                  | box read/write | value |
| -------------------------------------- | ------- | ----- |
| rhatcher/watson/state                  | RW | 'XXXX" |
| rhatcher/watson/Watson-MAC4            | -W | "client connected" or "client disconnected" |
| rhatcher/watson/Watson-MAC4/brightness | R- | <b1> <b2> <b3> <b4>   # values between 2 - 255 |
