//
// UrlRoutingModule.cs
//
// Author:
//	Atsushi Enomoto <atsushi@ximian.com>
//
// Copyright (C) 2008 Novell Inc. http://novell.com
//

//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
using System;
using System.Runtime.CompilerServices;
using System.Security.Permissions;
using System.Threading;
using System.Web;

namespace System.Web.Routing
{
	[TypeForwardedFrom ("System.Web.Routing, Version=3.5.0.0, Culture=Neutral, PublicKeyToken=31bf3856ad364e35")]
	[AspNetHostingPermission (SecurityAction.InheritanceDemand, Level = AspNetHostingPermissionLevel.Minimal)]
	[AspNetHostingPermission (SecurityAction.LinkDemand, Level = AspNetHostingPermissionLevel.Minimal)]
	public class UrlRoutingModule : IHttpModule
	{
		RouteCollection routes;
		public RouteCollection RouteCollection {
			get {
				if (routes == null)
					routes = RouteTable.Routes;
				return routes;
			}
			set { routes = value; }
		}

		protected virtual void Dispose ()
		{
		}

		void IHttpModule.Dispose ()
		{
			Dispose ();
		}

		void IHttpModule.Init (HttpApplication application)
		{
			Init (application);
		}

		protected virtual void Init (HttpApplication application)
		{
			application.PostMapRequestHandler += PostMapRequestHandler;
			application.PostResolveRequestCache += PostResolveRequestCache;
		}

		void PostMapRequestHandler (object o, EventArgs e)
		{
			var app = (HttpApplication) o;
			PostMapRequestHandler (new HttpContextWrapper (app.Context));
		}

		void PostResolveRequestCache (object o, EventArgs e)
		{
			var app = (HttpApplication) o;
			PostResolveRequestCache (new HttpContextWrapper (app.Context));
		}
		[Obsolete]
		public virtual void PostMapRequestHandler (HttpContextBase context)
		{
		}

		[MonoTODO]
		public virtual void PostResolveRequestCache (HttpContextBase context)
		{
			if (context == null)
				throw new ArgumentNullException ("context");

			var rd = RouteCollection.GetRouteData (context);
			if (rd == null)
				return; // do nothing

			if (rd.RouteHandler == null)
				throw new InvalidOperationException ("No  IRouteHandler is assigned to the selected route");

			if (rd.RouteHandler is StopRoutingHandler)
				return; //stop further processing
			
			var rc = new RequestContext (context, rd);

			IHttpHandler http = rd.RouteHandler.GetHttpHandler (rc);
			if (http == null)
				throw new InvalidOperationException ("The mapped IRouteHandler did not return an IHttpHandler");
			context.Request.RequestContext = rc;
			context.RemapHandler (http);
		}
	}
}
