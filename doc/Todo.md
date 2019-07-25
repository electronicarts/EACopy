* Fix so server connection is happening in parallel with copying files  
* Fix Authentication (Using SSPI with a Windows Sockets Client)  
* Add /SERVER:<host> that can be different than dest path (could be used as optimization overseas)  
* Robocopy exit codes? https://ss64.com/nt/robocopy-exit.html  
* Add local cache support on client side (to prevent re-copying when multiple servers produce each version). Server A creates version 1. Server B creates version 2. Server A creates version 3. 1 and 3 are similar, 2 is different.  
* EACopyService - Add rest api to ask for health status?  
