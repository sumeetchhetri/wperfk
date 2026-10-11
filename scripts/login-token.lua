-- Request sequence with state: log in, then reuse the token for every request
-- on that thread. (Connections that send before the first login response
-- arrives log in too.)
-- Expects the login response body to contain "token":"<value>".
--
--   wperfk -t2 -c10 -d30s -s scripts/login-token.lua https://api.example.com

local token = nil

request = function()
   if not token then
      return wrk.format("POST", "/login", { ["Content-Type"] = "application/json" },
                        '{"user":"demo","password":"demo"}')
   end
   return wrk.format("GET", "/profile", { ["Authorization"] = "Bearer " .. token })
end

response = function(status, headers, body)
   if not token and status == 200 then
      token = body:match('"token"%s*:%s*"([^"]+)"')
   end
end
