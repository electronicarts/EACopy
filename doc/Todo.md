* Add so server FindFiles command can do recursive traversal in one call
* Fix "session context" on server side which can keep track of created directories and use that info to avoid file info
* Robocopy exit codes? https://ss64.com/nt/robocopy-exit.html  
* Add local cache support on client side (to prevent re-copying when multiple servers produce each version). Server A creates version 1. Server B creates version 2. Server A creates version 3. 1 and 3 are similar, 2 is different.  
* EACopyService - Add rest api to ask for health status?  
