用c++写个用于高烈度pvp游戏的脚本，模拟和监控键鼠，请用hook层级的代码，每个按键监听需要并行。请用简洁的变量名。做好注释增加可读性，请为变量名注释命名的全英文来历。

全局条件
在DeltaForceClient - Win64 - Shipping.exe在前台时生效

功能1
建立一个状态器Num，记录用户输入的1，2，4，输入了哪个就切到哪个
按下鼠标侧键XButton2时循环：按下l松开l 等130ms 按下和松开Num对应数字  等20ms
松开鼠标侧键XButton2时结束循环


功能2
右键和h同步松开按下


功能3
f处于按下状态发且鼠标侧键1处于松开状态，循环：按下f松开f  等25ms
f处于按下状态发且鼠标侧键1处于按下状态，正常输出f
否则无事发生


功能4
按下空格时循环：spawn按下松开 ， 20ms delay


功能5
鼠标侧键1处于按下状态时，按下松开q只输出按下松开n，按下松开e只输出按下松开m；
鼠标侧键1处于松开状态时，q和a同步松开按下，e和d同步松开按下。


功能6
双模式按键映射逻辑规约(Spec)

1. 变量定义(State Definitions)
变量, 符号, 取值范围, 说明
模式, M, "{Shoulder, Scope}", 初始值：Shoulder
输出账本, O, "{U, RMB, None}", 记录当前逻辑模拟按下的键位
屏蔽位, S, "{True, False}", 初始值：False。用于拦截下一次释放信号

2. 原子子程序(Sub - routine)
$Reset()$:若 $O \neq None$：输出 $Release(O)$。令 $O \leftarrow None$。

3. 核心逻辑状态机表格(FSM Table)
输入事件(Event), 前置条件(Guard), 动作响应(Action / Response), 状态变更(Next State)
滚轮下滚, -, 1. 切换模式 M  2. 弹出 OSD 提示, M←(Shoulder↔Scope)
右键按下, -, 1. Block(拦截原生信号)  2. 执行 Reset()  3. 按下当前 M 对应的键, O←(U 或 RMB)
滚轮上滚, 物理右键松开, 1. 执行 Reset()  2. 按下当前 M 对应的键, O←(U 或 RMB)
滚轮上滚, 物理右键按下, (无输出动作), S←True
右键松开, S = True, 1. Block(拦截信号), S←False
右键松开, O = None, 1. Block(拦截信号)  2. 执行 Reset(), O←None

4. 初始化块(Initialization)
在脚本启动瞬间，必须执行以下操作以同步软硬件状态：令 $M \leftarrow Shoulder, O \leftarrow None, S \leftarrow False$。强制复位：输出一次 $Release(U)$ 和 $Release(RMB)$。


功能7
鼠标侧键1处于松开状态时，屏蔽用户c键输出，检测到c按下后，设定4秒计时，重复按下c刷新计时，4秒内如果鼠标右键处于按下状态或者检测到按下，那么按下“，”键


功能8
鼠标侧键1处于按下状态时，c键正常使用


功能9
鼠标侧键1处于按下状态时，按下松开x键按只输出按下松开“.”键


功能10
增加亮绿色十字准星，大小形状严格遵循如图所示!!!, 不透明度100% ，带有1像素宽度的黑色描边