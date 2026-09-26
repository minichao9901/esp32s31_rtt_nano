set confirm off
set pagination off
target remote :3333
printf "\n===== 驱动侧 =====\n"
printf "running=%d isr=%u pos=%u refill_pending=%u fs=%u bits=%u ch=%u\n", s_audio.running, s_audio.isr_cnt, s_audio.pos, s_refill_pending, s_audio.samplerate, s_audio.samplebits, s_audio.channels
printf "clk_target=%u period=%u\n", (unsigned)s_clk_target, s_clk_period
printf "\n===== 队列（get/put/is_empty）=====\n"
printf "get=%u put=%u empty=%d full=%d size=%u\n", s_audio.parent.replay->queue.get_index, s_audio.parent.replay->queue.put_index, s_audio.parent.replay->queue.is_empty, s_audio.parent.replay->queue.is_full, s_audio.parent.replay->queue.size
printf "replay pos=%u write_index=%u read_index=%u activated=%d\n", s_audio.parent.replay->pos, s_audio.parent.replay->write_index, s_audio.parent.replay->read_index, s_audio.parent.replay->activated
printf "\n===== 缓冲四段（各 16 字节，看有没有真数据）=====\n"
printf "tx[0]     : "
x/16ub s_audio.tx
printf "tx[512]   : "
x/16ub s_audio.tx+512
printf "tx[1024]  : "
x/16ub s_audio.tx+1024
printf "tx[1536]  : "
x/16ub s_audio.tx+1536
detach
quit
