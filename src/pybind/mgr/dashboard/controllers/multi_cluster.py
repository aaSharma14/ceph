from . import APIDoc, APIRouter, CreatePermission, UpdatePermission, ReadPermission, Endpoint, EndpointDoc, RESTController
import requests
from ..exceptions import DashboardException

from ..settings import Settings
from ..security import Scope

import logging
logger = logging.getLogger('routes')


@APIRouter('/multicluster', Scope.CONFIG_OPT)
@APIDoc('Multi Cluster Route Management API', 'Multi Cluster Route')
class MultiClusterRoute(RESTController):
    def _proxy(self, method, path, api_name, remote_cluster_url, apiToken, params=None, payload=None, verify=False):

        # {
        #     current_url: '',
        #     config: [{
        #         url: '',
        #     }]
        # }

        base_url = remote_cluster_url
        
        try:
            headers = {
                'Accept': 'application/vnd.ceph.api.v1.0+json',
                'Authorization': 'Bearer ' + apiToken
            }
            response = requests.request(method, base_url + '/' + path, params=params,
                                        json=payload, verify=verify, headers=headers)
            logger.error("the response is %s", response)
        except Exception as e:
            raise DashboardException(
                "Could not reach {}'s API on {}".format(api_name, base_url),
                http_status_code=404,
                component='dashboard')
        import json
        try:
            content = json.loads(response.content, strict=False)
        except json.JSONDecodeError as e:
            raise DashboardException(
                "Error parsing Dashboard API response: {}".format(e.msg),
                component='dashboard')
        return content



    @Endpoint()
    @ReadPermission
    @EndpointDoc("Which route you want to go")
    def route(self, path: str = '', method: str = 'GET', remote_cluster_url= '', apiToken='', params = None, payload = None):
        logger.info('remote_cluster_url is %s', remote_cluster_url)
        logger.info('apiToken is %s', apiToken)
        logger.info('params is %s', params)
        response = self._proxy(method, path, 'dashboard', remote_cluster_url, apiToken, params, payload=payload)
        return response

    @Endpoint('PUT')
    @CreatePermission
    def set_config(self, config: str):
        Settings.MULTICLUSTER_CONFIG['current_url'] = config
    
    @Endpoint()
    @ReadPermission
    def get_config(self):
        return Settings.MULTICLUSTER_CONFIG
