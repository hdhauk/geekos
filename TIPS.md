# Project 0
* You'll need `Copy_To_User(...)`

# Project 1
Implement `fork` and `exec`

* Parent and child should have same file descriptors.
    * Child inherit from parent.
* Child PID must be new, but sequential.
* Stack should be same at startup.

* `spawn` will make new process. We need to make memory copy etc.
* Want to make new `kthread` when we fork.

* Itrate through file_descriptor_array and update refrence count.