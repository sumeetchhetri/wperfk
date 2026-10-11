-- A unique id in every request body, for endpoints that reject duplicates.
--
--   wperfk -t4 -c50 -d30s -s scripts/unique-id.lua https://api.example.com/orders

local counter = 0
local prefix

init = function(args)
   -- distinct per thread (thread address) and per run (start time in us)
   prefix = string.format("%.0f-%s", wrk.time_us(), tostring(wrk.thread):match("0x%x+") or "t")
end

request = function()
   counter = counter + 1
   local body = string.format('{"id":"%s-%d","qty":1}', prefix, counter)
   return wrk.format("POST", nil, { ["Content-Type"] = "application/json" }, body)
end
