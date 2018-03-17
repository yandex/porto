import porto
from test_common import *

c = porto.Connection()

a = c.Create("a", weak=True)

ExpectEq(a["state"], "stopped")
ExpectEq(a["command"], "")
ExpectEq(Catch(a.GetProperty, "root_pid"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "exit_code"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "exit_status"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "oom_killed"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "core_dumped"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "memory_usage"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "cpu_usage"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "process_count"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "thread_count"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "stdout"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "stderr"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "stdout_offset"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "stderr_offset"), porto.exceptions.InvalidState)

a["command"] = "sleep 1000"
a.Start()

ExpectEq(a["state"], "running")
ExpectEq(a["command"], "sleep 1000")
ExpectNe(a["root_pid"], "0")
ExpectEq(Catch(a.GetProperty, "exit_code"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "exit_status"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "oom_killed"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "core_dumped"), porto.exceptions.InvalidState)
Expect(int(a["memory_usage"]) >= 0)
Expect(int(a["cpu_usage"]) >= 0)
ExpectEq(a["process_count"], "2")
ExpectEq(a["thread_count"], "2")
ExpectEq(a["stdout"], "")
ExpectEq(a["stderr"], "")

a.Kill(9)
a.Wait()

ExpectEq(a["state"], "dead")
ExpectEq(a["command"], "sleep 1000")
ExpectEq(Catch(a.GetProperty, "root_pid"), porto.exceptions.InvalidState)
ExpectEq(a["exit_code"], "-9")
ExpectEq(a["exit_status"], "9")
ExpectEq(a["oom_killed"], False)
ExpectEq(a["core_dumped"], False)
a["memory_usage"]
a["cpu_usage"]
ExpectEq(a["process_count"], "0")
Expect(int(a["thread_count"]) <= 2)
ExpectEq(a["stdout"], "")
ExpectEq(a["stderr"], "")

a.Stop()

ExpectEq(a["state"], "stopped")
ExpectEq(a["command"], "sleep 1000")
ExpectEq(Catch(a.GetProperty, "root_pid"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "exit_code"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "exit_status"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "oom_killed"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "core_dumped"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "memory_usage"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "cpu_usage"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "process_count"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "thread_count"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "stdout"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "stderr"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "stdout_offset"), porto.exceptions.InvalidState)
ExpectEq(Catch(a.GetProperty, "stderr_offset"), porto.exceptions.InvalidState)

a["command"] = "true"
a.Start()
a.Wait()
ExpectEq(a["exit_code"], "0")
ExpectEq(a["exit_status"], "0")
ExpectEq(a["oom_killed"], False)
ExpectEq(a["core_dumped"], False)
ExpectEq(a["stdout"], "")
ExpectEq(a["stderr"], "")
a.Stop()

a["command"] = "false"
a.Start()
a.Wait()
ExpectEq(a["exit_code"], "1")
ExpectEq(a["exit_status"], "256")
ExpectEq(a["oom_killed"], False)
ExpectEq(a["core_dumped"], False)
ExpectEq(a["stdout"], "")
ExpectEq(a["stderr"], "")
a.Stop()

a["command"] = "printf %s $PORTO_NAME"
a.Start()
a.Wait()
ExpectEq(a["exit_code"], "0")
ExpectEq(a["stdout"], "a")
ExpectEq(a["stderr"], "")
ExpectEq(a["stdout_offset"], "0")
ExpectEq(a["stderr_offset"], "0")
a.Stop()

a["command"] = "sh -c 'echo test 1>&2'"
a.Start()
a.Wait()
ExpectEq(a["exit_code"], "0")
ExpectEq(a["stdout"], "")
ExpectEq(a["stderr"], "test\n")
a.Stop()

a["command"] = "sh -e -c 'func() { true ;}; func; (true); true < /dev/null | true > /dev/null; true\necho test $PORTO_NAME'"
a.Start()
a.Wait()
ExpectEq(a["exit_code"], "0")
ExpectEq(a["exit_status"], "0")
ExpectEq(a["oom_killed"], False)
ExpectEq(a["core_dumped"], False)
ExpectEq(a["stdout"], "test a\n")
ExpectEq(a["stderr"], "")
a.Stop()

a["command"] = "seq 4096"
a["stdout_limit"] = "4096"
a.Start()
a.Wait()
ExpectEq(a["exit_code"], "0")
ExpectEq(a["exit_status"], "0")
ExpectEq(a["oom_killed"], False)
ExpectEq(a["core_dumped"], False)
ExpectEq(a["stdout_offset"], "16384")
ExpectEq(a["stderr_offset"], "0")
ExpectEq(Catch(a.GetProperty, "stdout[0:]"), porto.exceptions.InvalidData)
ExpectEq(len(a["stdout"]), 2989)
ExpectEq(a["stdout[16384:10]"], "499\n3500\n3")
ExpectEq(a["stdout[:10]"], "4095\n4096\n")
ExpectEq(a["stderr"], "")
a.Stop()

a["command"] = "__non_existing_command__"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)
ExpectEq(a["state"], "stopped")
ExpectEq(a["command"], "__non_existing_command__")

a["command"] = "true \n false"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)

a["command"] = "true ; false"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)

a["command"] = "true | false"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)

a["command"] = "false &"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)

a["command"] = "(false)"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)

a["command"] = "false </dev/null"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)

a["command"] = "false >/dev/full"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)

a["command"] = "false $FOO"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)

a["command"] = "false $(bar)"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)

a["command"] = "false `bar`"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)

a["command"] = "func() {false}; func"
ExpectEq(Catch(a.Start), porto.exceptions.InvalidCommand)

a.Destroy()
