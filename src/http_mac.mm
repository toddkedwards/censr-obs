/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

/* macOS HTTPS via NSURLSession, run synchronously behind a semaphore. */

#include "http.h"

#import <Foundation/Foundation.h>

namespace censr {

bool https_post(const std::string &url, const std::string &content_type,
		const std::string &body, long &out_status, std::string &out_response)
{
	out_status = 0;
	out_response.clear();
	@autoreleasepool {
		NSString *us = [NSString stringWithUTF8String:url.c_str()];
		NSURL *u = us ? [NSURL URLWithString:us] : nil;
		if (!u)
			return false;

		NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:u];
		req.HTTPMethod = @"POST";
		req.timeoutInterval = 15;
		[req setValue:[NSString stringWithUTF8String:content_type.c_str()]
			forHTTPHeaderField:@"Content-Type"];
		[req setValue:@"application/json" forHTTPHeaderField:@"Accept"];
		req.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];

		__block long status = 0;
		__block std::string resp;
		__block bool ok = false;
		dispatch_semaphore_t sem = dispatch_semaphore_create(0);

		NSURLSessionDataTask *task = [[NSURLSession sharedSession]
			dataTaskWithRequest:req
			  completionHandler:^(NSData *data, NSURLResponse *response,
					      NSError *error) {
				  if (!error &&
				      [response isKindOfClass:[NSHTTPURLResponse class]]) {
					  status = (long)[(NSHTTPURLResponse *)response
						  statusCode];
					  if (data.length)
						  resp.assign((const char *)data.bytes,
							      data.length);
					  ok = true;
				  }
				  dispatch_semaphore_signal(sem);
			  }];
		[task resume];
		dispatch_semaphore_wait(
			sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)20 * NSEC_PER_SEC));

		out_status = status;
		out_response = resp;
		return ok;
	}
}

} // namespace censr
